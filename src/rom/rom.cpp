#include "rom/rom.h"

#include <fstream>
#include <system_error>
#include <utility>

#include "core/checksum.h"
#include "rom/compression.h"

namespace bd::rom {
namespace {

// The largest DS cartridge is 512 MiB. Refuse anything far beyond that before
// trying to read it into memory.
constexpr std::uint64_t kMaxRomSize = 1024ull * 1024 * 1024;
constexpr std::uint32_t kSecureAreaStart = 0x4000;
constexpr std::uint32_t kSecureAreaEnd = 0x8000;

Status check_code_section(ByteSpan rom, const CodeSection& section, const char* which) {
  if (section.size == 0)
    return Error{ErrorCode::Corrupt, std::string(which) + " program is empty"};
  if (!in_range(rom.size(), section.rom_offset, section.size)) {
    return Error{ErrorCode::Truncated,
                 std::string(which) + " program extends past the end of the image"};
  }
  return {};
}

}  // namespace

Result<Rom> Rom::open(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status))
    return Error{ErrorCode::Io, "ROM file does not exist: " + path.string()};
  if (!std::filesystem::is_regular_file(status))
    return Error{ErrorCode::Io, "not a regular file: " + path.string()};
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return Error{ErrorCode::Io, "cannot determine the size of " + path.string()};
  if (size > kMaxRomSize)
    return Error{ErrorCode::NotNds, "file is larger than any Nintendo DS cartridge"};

  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot open " + path.string() + " for reading"};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()))) {
    return Error{ErrorCode::Io, "failed while reading " + path.string()};
  }
  return from_bytes(std::move(bytes));
}

Result<Rom> Rom::from_bytes(std::vector<std::uint8_t> bytes) {
  Rom rom;
  rom.bytes_ = std::move(bytes);
  const ByteSpan data = rom.bytes_;

  auto header = parse_header(data);
  if (!header) return header.error();
  rom.header_ = std::move(header.value());
  const NdsHeader& h = rom.header_;

  if (h.used_rom_size > data.size()) {
    return Error{ErrorCode::Truncated,
                 "the header says the ROM holds " + std::to_string(h.used_rom_size) +
                     " bytes of data but the file is only " + std::to_string(data.size()) +
                     " bytes; the dump is incomplete"};
  }
  if (auto s = check_code_section(data, h.arm9, "ARM9"); !s) return s.error();
  if (auto s = check_code_section(data, h.arm7, "ARM7"); !s) return s.error();

  auto fs = NitroFs::parse(data, h);
  if (!fs) return fs.error();
  rom.fs_ = std::move(fs.value());

  auto ov9 = parse_overlay_table(data, h.arm9_overlays, rom.fs_.ranges().size());
  if (!ov9) return ov9.error();
  rom.arm9_overlays_ = std::move(ov9.value());
  auto ov7 = parse_overlay_table(data, h.arm7_overlays, rom.fs_.ranges().size());
  if (!ov7) return ov7.error();
  rom.arm7_overlays_ = std::move(ov7.value());

  rom.module_params_ = find_module_params(rom.arm9());

  // A dumped ROM normally has its secure area decrypted, which leaves a fixed
  // marker in the first eight bytes.
  if (h.arm9.rom_offset >= kSecureAreaStart && h.arm9.rom_offset < kSecureAreaEnd &&
      h.arm9.size >= 8) {
    const ByteSpan code = rom.arm9();
    const bool decrypted = le32(code, 0) == 0xE7FFDEFFu && le32(code, 4) == 0xE7FFDEFFu;
    rom.secure_area_ = decrypted ? SecureArea::Decrypted : SecureArea::Encrypted;
    if (!decrypted) {
      rom.warnings_.push_back(
          "the secure area does not carry the usual decrypted marker; the first 2 KiB of "
          "ARM9 code may be encrypted");
    }
  }

  if (crc16(data.subspan(0x0C0, 0x9C)) != h.logo_crc) {
    rom.warnings_.push_back("the logo checksum in the header does not match the logo data");
  } else if (h.logo_crc != NdsHeader::kOfficialLogoCrc) {
    rom.warnings_.push_back("the header does not carry the logo found on retail cartridges");
  }
  if (!rom.module_params_) {
    rom.warnings_.push_back(
        "SDK start-up parameters were not found in the ARM9 program; it is assumed to be "
        "uncompressed");
  }
  return rom;
}

ByteSpan Rom::arm9() const {
  return ByteSpan(bytes_).subspan(header_.arm9.rom_offset, header_.arm9.size);
}

ByteSpan Rom::arm7() const {
  return ByteSpan(bytes_).subspan(header_.arm7.rom_offset, header_.arm7.size);
}

Result<std::vector<std::uint8_t>> Rom::arm9_decompressed() const {
  return decompress_arm9(arm9(), header_.arm9, module_params_);
}

Result<OverlayImage> Rom::load_overlay(const Overlay& overlay) const {
  auto raw = file(overlay.file_id);
  if (!raw) return raw.error();

  if (overlay.compressed) {
    auto decoded = blz_decompress(raw.value());
    if (!decoded) {
      return Error{decoded.error().code, "cannot decompress overlay " +
                                             std::to_string(overlay.id) + ": " +
                                             decoded.error().message};
    }
    return OverlayImage{std::move(decoded.value()), OverlayEncoding::Blz};
  }

  OverlayImage image{std::vector<std::uint8_t>(raw.value().begin(), raw.value().end()),
                     OverlayEncoding::Raw};
  // Unflagged overlays may be LZ40 files, possibly more than one layer deep.
  // Stop as soon as the size matches what the overlay table says is loaded.
  constexpr int kMaxLayers = 4;
  for (int layer = 0; layer < kMaxLayers && image.data.size() != overlay.ram_size; ++layer) {
    if (image.data.empty() || image.data[0] != 0x40) break;
    auto decoded = lz40_decompress_file(image.data);
    if (!decoded) break;
    image.data = std::move(decoded.value());
    image.encoding = OverlayEncoding::Lz40;
  }
  return image;
}

Result<ByteSpan> Rom::file(std::uint16_t file_id) const {
  const auto& ranges = fs_.ranges();
  if (file_id >= ranges.size())
    return Error{ErrorCode::NotFound, "no file with ID " + std::to_string(file_id)};
  return ByteSpan(bytes_).subspan(ranges[file_id].offset, ranges[file_id].size);
}

}  // namespace bd::rom
