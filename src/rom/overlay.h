// Overlays are pieces of program code that the game loads into memory on
// demand. Several overlays may be linked to the same address range, which is
// one of the main difficulties for static recompilation.
#pragma once

#include <cstdint>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/nds_header.h"

namespace bd::rom {

struct Overlay {
  std::uint32_t id = 0;
  std::uint32_t ram_address = 0;
  std::uint32_t ram_size = 0;  // Size of the code and data once loaded.
  std::uint32_t bss_size = 0;
  std::uint32_t static_init_start = 0;
  std::uint32_t static_init_end = 0;
  std::uint16_t file_id = 0;
  std::uint32_t compressed_size = 0;
  bool compressed = false;
};

// Reads an overlay table. `file_count` is the number of FAT entries, used to
// reject entries that point at files which do not exist.
Result<std::vector<Overlay>> parse_overlay_table(ByteSpan rom, TableRef table,
                                                 std::size_t file_count);

}  // namespace bd::rom
