// Nintendo DS cartridge header. Field layout follows the publicly documented
// cartridge format (GBATEK, "DS Cartridge Header").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::rom {

// One executable image (ARM9 or ARM7) as placed in the ROM and in memory.
struct CodeSection {
  std::uint32_t rom_offset = 0;
  std::uint32_t entry_address = 0;
  std::uint32_t ram_address = 0;
  std::uint32_t size = 0;
};

struct TableRef {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

struct NdsHeader {
  // Number of bytes covered by the header checksum plus the checksum itself.
  static constexpr std::size_t kChecksummedSize = 0x15E;
  static constexpr std::size_t kMinSize = 0x160;
  static constexpr std::uint16_t kOfficialLogoCrc = 0xCF56;

  std::string title;       // Up to 12 characters, trailing NULs removed.
  std::string game_code;   // 4 characters, e.g. "BO5E".
  std::string maker_code;  // 2 characters.
  std::uint8_t unit_code = 0;        // 0 = DS, 2 = DS + DSi, 3 = DSi only.
  std::uint8_t device_capacity = 0;  // Chip size is 128 KiB << capacity.
  std::uint8_t rom_version = 0;

  CodeSection arm9;
  CodeSection arm7;
  TableRef fnt;            // File name table.
  TableRef fat;            // File allocation table.
  TableRef arm9_overlays;  // Overlay table for the main processor.
  TableRef arm7_overlays;

  std::uint32_t banner_offset = 0;
  std::uint16_t secure_area_crc = 0;
  std::uint32_t used_rom_size = 0;  // Bytes of the image that carry data.
  std::uint32_t header_size = 0;

  std::uint16_t logo_crc = 0;    // As stored in the header.
  std::uint16_t header_crc = 0;  // As stored in the header.

  std::uint64_t chip_size() const { return (std::uint64_t{128} * 1024) << (device_capacity & 0x1F); }
};

// Parses and checksums the header at the start of `rom`. Fails with NotNds when
// the data does not carry a self-consistent DS header.
Result<NdsHeader> parse_header(ByteSpan rom);

}  // namespace bd::rom
