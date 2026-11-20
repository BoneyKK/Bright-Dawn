// The code Bright Dawn puts where the DS main processor's BIOS would be.
//
// It is not Nintendo's BIOS and contains none of it: it is this project's
// own code, assembled from bios_rom.S, for the small part of a BIOS that has
// to run on the processor (the interrupt entry and the calls that wait for an
// interrupt). Everything else a BIOS does is done natively in bios.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "core/result.h"

namespace bd::nds {

// Where the code goes, and the addresses in it that the console uses.
inline constexpr std::uint32_t kBiosRomAddress = 0xFFFF0000;
inline constexpr std::uint32_t kBiosCallVector = kBiosRomAddress + 0x08;
inline constexpr std::uint32_t kBiosInterruptVector = kBiosRomAddress + 0x18;

// The assembled code, as 32-bit little-endian words. A test assembles
// bios_rom.S and fails if the result differs from this.
extern const std::uint32_t kBiosRom[];
extern const std::size_t kBiosRomWords;

// Puts this code with the game's in a folder of program code for the code
// translator (rom/program_code.h): bios9.bin, and a "bios9" line in its
// manifest.txt, which must be there already, starting at the interrupt
// entry. So the game library holds the interrupt entry as native code too.
// Returns how many files were written.
Result<std::size_t> add_bios_code(const std::filesystem::path& folder);

}  // namespace bd::nds
