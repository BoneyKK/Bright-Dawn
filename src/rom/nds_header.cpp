#include "rom/nds_header.h"

#include <cstdio>

#include "core/checksum.h"

namespace bd::rom {
namespace {

std::string fixed_string(ByteSpan data, std::size_t offset, std::size_t length) {
  std::string out(reinterpret_cast<const char*>(data.data() + offset), length);
  while (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

CodeSection code_section(ByteSpan data, std::size_t offset) {
  return CodeSection{le32(data, offset), le32(data, offset + 4), le32(data, offset + 8),
                     le32(data, offset + 12)};
}

TableRef table_ref(ByteSpan data, std::size_t offset) {
  return TableRef{le32(data, offset), le32(data, offset + 4)};
}

bool starts_with(ByteSpan data, std::initializer_list<std::uint8_t> magic) {
  if (data.size() < magic.size()) return false;
  std::size_t i = 0;
  for (std::uint8_t byte : magic) {
    if (data[i++] != byte) return false;
  }
  return true;
}

// People frequently point a ROM picker at the archive their dump lives in.
const char* archive_kind(ByteSpan data) {
  if (starts_with(data, {'P', 'K', 0x03, 0x04})) return "ZIP";
  if (starts_with(data, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) return "7-Zip";
  if (starts_with(data, {'R', 'a', 'r', '!'})) return "RAR";
  if (starts_with(data, {0x1F, 0x8B})) return "gzip";
  return nullptr;
}

}  // namespace

Result<NdsHeader> parse_header(ByteSpan rom) {
  if (const char* kind = archive_kind(rom)) {
    return Error{ErrorCode::NotNds, std::string("this file is a ") + kind +
                                        " archive; extract the .nds file from it first"};
  }
  if (rom.size() < NdsHeader::kMinSize) {
    return Error{ErrorCode::NotNds, "file is too small to contain a Nintendo DS header"};
  }

  NdsHeader h;
  h.header_crc = le16(rom, 0x15E);
  const std::uint16_t computed = crc16(rom.first(NdsHeader::kChecksummedSize));
  if (computed != h.header_crc) {
    char text[160];
    std::snprintf(text, sizeof text,
                  "header checksum mismatch (stored %04X, computed %04X): this is not a "
                  "Nintendo DS ROM image, or its header is damaged",
                  static_cast<unsigned>(h.header_crc), static_cast<unsigned>(computed));
    return Error{ErrorCode::NotNds, text};
  }

  h.title = fixed_string(rom, 0x000, 12);
  h.game_code = fixed_string(rom, 0x00C, 4);
  h.maker_code = fixed_string(rom, 0x010, 2);
  h.unit_code = rom[0x012];
  h.device_capacity = rom[0x014];
  h.rom_version = rom[0x01E];
  h.arm9 = code_section(rom, 0x020);
  h.arm7 = code_section(rom, 0x030);
  h.fnt = table_ref(rom, 0x040);
  h.fat = table_ref(rom, 0x048);
  h.arm9_overlays = table_ref(rom, 0x050);
  h.arm7_overlays = table_ref(rom, 0x058);
  h.banner_offset = le32(rom, 0x068);
  h.secure_area_crc = le16(rom, 0x06C);
  h.used_rom_size = le32(rom, 0x080);
  h.header_size = le32(rom, 0x084);
  h.logo_crc = le16(rom, 0x15C);
  return h;
}

}  // namespace bd::rom
