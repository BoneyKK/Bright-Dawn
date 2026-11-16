#include "assets/font.h"

#include <algorithm>
#include <string>

#include "rom/compression.h"

namespace bd::assets {
namespace {

Error not_a_font(const std::string& why) { return Error{ErrorCode::Corrupt, "not a font: " + why}; }

// Little-endian reads that fail outside the data.
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
  bool has(std::size_t at, std::size_t size) const { return at <= data_.size() && size <= data_.size() - at; }
  unsigned u8(std::size_t at) const { return data_[at]; }
  unsigned u16(std::size_t at) const { return data_[at] | (static_cast<unsigned>(data_[at + 1]) << 8); }
  std::uint32_t u32(std::size_t at) const { return u16(at) | (static_cast<std::uint32_t>(u16(at + 2)) << 16); }
  bool tag(std::size_t at, const char* four) const {
    return has(at, 4) && std::equal(four, four + 4, data_.begin() + static_cast<std::ptrdiff_t>(at));
  }

 private:
  const std::vector<std::uint8_t>& data_;
};

constexpr std::size_t kMaxBlocks = 4096;  // Width and map blocks chained; more means a loop.

}  // namespace

Result<Font> Font::parse(ByteSpan file) {
  std::vector<std::uint8_t> data(file.begin(), file.end());
  if (!data.empty() && data[0] == 0x40) {
    auto decoded = rom::lz40_decompress_file(file);
    if (!decoded) return not_a_font("its compression: " + decoded.error().message);
    data = std::move(decoded.value());
  }
  const Reader r(data);
  // The file header: "RTFN", a byte-order mark, the version, the size, the
  // header's size and the number of blocks.
  if (!r.tag(0, "RTFN") || !r.has(0, 16)) return not_a_font("no NFTR header");
  if (r.u16(4) != 0xFEFF) return not_a_font("big-endian");
  const std::size_t finf = r.u16(12);
  // The font information: the line height, the encoding and where the other
  // blocks' contents start (each 8 bytes after its block's tag).
  if (!r.tag(finf, "FNIF") || !r.has(finf, 28)) return not_a_font("no FINF block");
  Font font;
  font.line_height_ = static_cast<int>(r.u8(finf + 9));
  const unsigned encoding = r.u8(finf + 15);
  if (encoding != 1) return not_a_font("encoding " + std::to_string(encoding) + ", not UTF-16");
  const std::size_t glyphs_at = r.u32(finf + 16);
  std::size_t widths_at = r.u32(finf + 20);
  std::size_t map_at = r.u32(finf + 24);

  // The glyphs: the cell's size, the bytes per glyph, the baseline and the
  // bits per pixel; then the bitmaps, pixels in rows, the first in the
  // highest bits of a byte, rows not padded to bytes.
  if (glyphs_at < 8 || !r.tag(glyphs_at - 8, "PLGC") || !r.has(glyphs_at, 8)) return not_a_font("no CGLP block");
  font.cell_width_ = static_cast<int>(r.u8(glyphs_at));
  font.cell_height_ = static_cast<int>(r.u8(glyphs_at + 1));
  const std::size_t cell_bytes = r.u16(glyphs_at + 2);
  font.baseline_ = static_cast<int>(r.u8(glyphs_at + 4));
  font.bits_ = static_cast<int>(r.u8(glyphs_at + 6));
  if (font.bits_ != 1 && font.bits_ != 2 && font.bits_ != 4 && font.bits_ != 8)
    return not_a_font(std::to_string(font.bits_) + " bits a pixel");
  const std::size_t cell_pixels = static_cast<std::size_t>(font.cell_width_) * static_cast<std::size_t>(font.cell_height_);
  if (cell_pixels == 0 || cell_bytes * 8 < cell_pixels * static_cast<std::size_t>(font.bits_))
    return not_a_font("cells of " + std::to_string(cell_bytes) + " bytes for " + std::to_string(font.cell_width_) +
                      " by " + std::to_string(font.cell_height_) + " pixels");
  const std::size_t block_size = r.u32(glyphs_at - 4);
  if (block_size < 16 || !r.has(glyphs_at - 8, block_size)) return not_a_font("the CGLP block runs past the end");
  const std::size_t count = (block_size - 16) / cell_bytes;
  font.glyphs_.resize(count);
  const unsigned mask = (1u << font.bits_) - 1;
  for (std::size_t g = 0; g < count; ++g) {
    Glyph& glyph = font.glyphs_[g];
    glyph.pixels.resize(cell_pixels);
    const std::size_t base = glyphs_at + 8 + g * cell_bytes;
    for (std::size_t p = 0; p < cell_pixels; ++p) {
      const std::size_t bit = p * static_cast<std::size_t>(font.bits_);
      const unsigned byte = r.u8(base + bit / 8);
      glyph.pixels[p] = static_cast<std::uint8_t>((byte >> (8 - font.bits_ - bit % 8)) & mask);
    }
    // Until the widths say otherwise: the whole cell.
    glyph.width = glyph.advance = font.cell_width_;
  }

  // The widths: blocks of glyphs first..last, three bytes each.
  for (std::size_t n = 0; widths_at != 0; ++n) {
    if (n == kMaxBlocks) return not_a_font("CWDH blocks that never end");
    if (widths_at < 8 || !r.tag(widths_at - 8, "HDWC") || !r.has(widths_at, 8)) return not_a_font("a CWDH block");
    const std::size_t first = r.u16(widths_at), last = r.u16(widths_at + 2);
    if (last < first || !r.has(widths_at + 8, (last - first + 1) * 3)) return not_a_font("a CWDH block's range");
    for (std::size_t g = first; g <= last && g < count; ++g) {
      const std::size_t at = widths_at + 8 + (g - first) * 3;
      font.glyphs_[g].left = static_cast<std::int8_t>(r.u8(at));
      font.glyphs_[g].width = static_cast<int>(r.u8(at + 1));
      font.glyphs_[g].advance = static_cast<int>(r.u8(at + 2));
    }
    widths_at = r.u32(widths_at + 4);
  }

  // The character maps: a run of codes to a run of glyphs, a table, or a
  // list of pairs.
  const auto add = [&](std::uint32_t code, std::uint32_t glyph) {
    if (glyph != 0xFFFF && glyph < count) font.map_.emplace(static_cast<char32_t>(code), glyph);
  };
  for (std::size_t n = 0; map_at != 0; ++n) {
    if (n == kMaxBlocks) return not_a_font("CMAP blocks that never end");
    if (map_at < 8 || !r.tag(map_at - 8, "PAMC") || !r.has(map_at, 12)) return not_a_font("a CMAP block");
    const std::uint32_t first = r.u16(map_at), last = r.u16(map_at + 2);
    const unsigned kind = r.u16(map_at + 4);
    const std::size_t entries = map_at + 12;
    if (kind == 0) {
      if (!r.has(entries, 2) || last < first) return not_a_font("a CMAP block's range");
      for (std::uint32_t c = first; c <= last; ++c) add(c, r.u16(entries) + (c - first));
    } else if (kind == 1) {
      if (last < first || !r.has(entries, (last - first + 1) * 2)) return not_a_font("a CMAP table");
      for (std::uint32_t c = first; c <= last; ++c) add(c, r.u16(entries + (c - first) * 2));
    } else if (kind == 2) {
      if (!r.has(entries, 2)) return not_a_font("a CMAP list");
      const std::size_t pairs = r.u16(entries);
      if (!r.has(entries + 2, pairs * 4)) return not_a_font("a CMAP list runs past the end");
      for (std::size_t i = 0; i < pairs; ++i) add(r.u16(entries + 2 + i * 4), r.u16(entries + 4 + i * 4));
    } else {
      return not_a_font("a CMAP block of kind " + std::to_string(kind));
    }
    map_at = r.u32(map_at + 8);
  }
  if (font.map_.empty()) return not_a_font("no characters");
  return font;
}

const Glyph* Font::find(char32_t code) const {
  const auto it = map_.find(code);
  return it == map_.end() ? nullptr : &glyphs_[it->second];
}

std::vector<char32_t> Font::characters() const {
  std::vector<char32_t> codes;
  codes.reserve(map_.size());
  for (const auto& [code, glyph] : map_) codes.push_back(code);
  std::sort(codes.begin(), codes.end());
  return codes;
}

Result<Font> read_font(const rom::Rom& rom, std::string_view path) {
  const rom::NamedFile* named = rom.fs().find(path);
  if (named == nullptr) return Error{ErrorCode::NotFound, "the ROM has no " + std::string(path)};
  auto file = rom.file(named->file_id);
  if (!file) return file.error();
  auto font = Font::parse(file.value());
  if (!font) return Error{font.error().code, std::string(path) + ": " + font.error().message};
  return font;
}

}  // namespace bd::assets
