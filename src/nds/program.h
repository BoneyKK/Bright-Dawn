// A game's program code, ready to be placed in memory: the main program with
// its compression removed, and its overlays.
//
// It comes either straight from the ROM or from a directory written by
// `bdtool extract --code`. Either way it is the user's own data and is never
// part of this repository.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/result.h"
#include "rom/rom.h"

namespace bd::nds {

struct OverlayCode {
  int id = 0;
  std::uint32_t load_address = 0;
  std::uint32_t bss_size = 0;
  std::uint32_t static_init_start = 0, static_init_end = 0;
  std::vector<std::uint8_t> image;  // As it sits in memory once loaded.
};

struct Program {
  std::vector<std::uint8_t> arm9;  // Decompressed.
  std::uint32_t arm9_load_address = 0;
  std::uint32_t arm9_entry = 0;
  // The start of the ROM header, which the console copies into memory for the
  // program to read. Empty when the source does not have it.
  std::vector<std::uint8_t> header;
  std::vector<OverlayCode> overlays;
};

Result<Program> program_from_rom(const rom::Rom& rom);

// `directory` holds manifest.txt and the files it names.
Result<Program> program_from_directory(const std::filesystem::path& directory);

}  // namespace bd::nds
