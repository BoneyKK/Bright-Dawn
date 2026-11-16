// Fonts in the DS's NFTR format (assets/font.h), from fonts made up here.
#include <cstdint>
#include <vector>

#include "assets/font.h"
#include "codecs.h"
#include "test.h"

namespace {

using bd_test::Bytes;

void put16(Bytes& b, std::size_t at, unsigned v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
  put16(b, at, v & 0xFFFF);
  put16(b, at + 2, v >> 16);
}

// Appends a block: its tag, its size, then `body`. Returns where the body
// starts, which is where the other blocks point.
std::size_t block(Bytes& file, const char* tag, const Bytes& body) {
  const std::size_t at = file.size();
  file.insert(file.end(), tag, tag + 4);
  file.resize(file.size() + 4);
  put32(file, at + 4, static_cast<std::uint32_t>(8 + body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return at + 8;
}

// A font of 3 by 2 cells at one bit a pixel: glyph 0 blank (the space),
// glyph 1 "A" (101 / 010). ' ' and 'A' by a run and a table, U+3042 by a
// list of pairs; '!' is in the table but has no glyph.
struct MadeUp {
  Bytes file;
  std::size_t first_map = 0;  // Where the first character map's body is.
};

MadeUp made_up_font(unsigned bits = 1) {
  MadeUp f;
  Bytes& file = f.file;
  file.resize(16);
  file[0] = 'R', file[1] = 'T', file[2] = 'F', file[3] = 'N';
  put16(file, 4, 0xFEFF);
  put16(file, 6, 0x0100);
  put16(file, 12, 16);  // Where the font information is.
  put16(file, 14, 6);
  Bytes info(20, 0);
  info[1] = 2;  // The line's height.
  info[7] = 1;  // UTF-16.
  const std::size_t info_at = block(file, "FNIF", info);

  const std::size_t cell_bytes = bits == 1 ? 1 : 2;
  Bytes glyphs(8, 0);
  glyphs[0] = 3;  // Cell width,
  glyphs[1] = 2;  // height,
  glyphs[2] = static_cast<std::uint8_t>(cell_bytes);
  glyphs[4] = 1;  // the baseline,
  glyphs[6] = static_cast<std::uint8_t>(bits);
  for (std::size_t i = 0; i < cell_bytes; ++i) glyphs.push_back(0);  // The space.
  if (bits == 1) {
    glyphs.push_back(0xA8);  // 101010, the first pixel in the highest bit.
  } else {
    glyphs.push_back(0xCC);  // 11 00 11 00 | 11 00: full, blank, full, blank, full, blank.
    glyphs.push_back(0xC0);
  }
  const std::size_t glyphs_at = block(file, "PLGC", glyphs);

  Bytes widths = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 1, 2, 4};
  const std::size_t widths_at = block(file, "HDWC", widths);

  // A run: 'A' is glyph 1.
  Bytes run = {0x41, 0, 0x41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  f.first_map = block(file, "PAMC", run);
  // A table: ' ' is glyph 0, '!' none.
  Bytes table = {0x20, 0, 0x21, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  const std::size_t table_at = block(file, "PAMC", table);
  // A list: U+3042 is glyph 1.
  Bytes list = {0, 0, 0xFF, 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0x42, 0x30, 1, 0};
  const std::size_t list_at = block(file, "PAMC", list);
  put32(file, f.first_map + 8, static_cast<std::uint32_t>(table_at));
  put32(file, table_at + 8, static_cast<std::uint32_t>(list_at));

  put32(file, info_at + 8, static_cast<std::uint32_t>(glyphs_at));
  put32(file, info_at + 12, static_cast<std::uint32_t>(widths_at));
  put32(file, info_at + 16, static_cast<std::uint32_t>(f.first_map));
  put32(file, 8, static_cast<std::uint32_t>(file.size()));
  return f;
}

}  // namespace

TEST(font_is_read_with_its_glyphs_widths_and_characters) {
  const MadeUp f = made_up_font();
  auto font = bd::assets::Font::parse(f.file);
  CHECK(font.ok());
  if (!font) return;
  const bd::assets::Font& v = font.value();
  CHECK(v.cell_width() == 3 && v.cell_height() == 2);
  CHECK(v.line_height() == 2 && v.baseline() == 1 && v.max_value() == 1);
  CHECK(v.glyph_count() == 2);
  const bd::assets::Glyph* a = v.find(U'A');
  CHECK(a != nullptr);
  if (a == nullptr) return;
  CHECK((a->pixels == std::vector<std::uint8_t>{1, 0, 1, 0, 1, 0}));
  CHECK(a->left == 1 && a->width == 2 && a->advance == 4);
  const bd::assets::Glyph* space = v.find(U' ');
  CHECK(space != nullptr && space->advance == 3 && space->width == 0);
  CHECK(v.find(U'!') == nullptr);  // In the table, without a glyph.
  CHECK(v.find(U'B') == nullptr);
  CHECK(v.find(U'あ') == a);
  CHECK((v.characters() == std::vector<char32_t>{U' ', U'A', U'あ'}));
}

TEST(font_with_two_bits_a_pixel_keeps_its_values) {
  auto font = bd::assets::Font::parse(made_up_font(2).file);
  CHECK(font.ok());
  if (!font) return;
  CHECK(font.value().max_value() == 3);
  const bd::assets::Glyph* a = font.value().find(U'A');
  CHECK(a != nullptr && (a->pixels == std::vector<std::uint8_t>{3, 0, 3, 0, 3, 0}));
}

TEST(font_is_read_compressed) {
  const MadeUp f = made_up_font();
  auto font = bd::assets::Font::parse(bd_test::lz40_compress(f.file));
  CHECK(font.ok());
  CHECK(font.ok() && font.value().find(U'A') != nullptr);
}

TEST(font_refuses_what_is_not_one) {
  const MadeUp f = made_up_font();
  // Not a font at all.
  CHECK(!bd::assets::Font::parse(Bytes(64, 0)).ok());
  CHECK(!bd::assets::Font::parse(Bytes{}).ok());
  // Cut short, at every length: never read past the end.
  for (std::size_t n = 0; n < f.file.size(); ++n) {
    const Bytes cut(f.file.begin(), f.file.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK(!bd::assets::Font::parse(cut).ok());
  }
  // Character maps that lead back to themselves.
  Bytes loop = f.file;
  put32(loop, f.first_map + 8, static_cast<std::uint32_t>(f.first_map));
  auto looped = bd::assets::Font::parse(loop);
  CHECK(!looped.ok());
  // Another encoding than UTF-16.
  Bytes other = f.file;
  other[16 + 8 + 7] = 0;
  CHECK(!bd::assets::Font::parse(other).ok());
  // Bits a pixel the DS does not have.
  Bytes three = f.file;
  three[16 + 28 + 8 + 6] = 3;
  CHECK(!bd::assets::Font::parse(three).ok());
}
