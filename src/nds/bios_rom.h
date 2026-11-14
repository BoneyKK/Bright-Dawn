// The code Bright Dawn puts where the DS main processor's BIOS would be.
//
// It is not Nintendo's BIOS and contains none of it: it is this project's
// own code, assembled from bios_rom.S, for the small part of a BIOS that has
// to run on the processor (the interrupt entry and the calls that wait for an
// interrupt). Everything else a BIOS does is done natively in bios.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::nds {

// Where the code goes, and the addresses in it that the console uses.
inline constexpr std::uint32_t kBiosRomAddress = 0xFFFF0000;
inline constexpr std::uint32_t kBiosCallVector = kBiosRomAddress + 0x08;
inline constexpr std::uint32_t kBiosInterruptVector = kBiosRomAddress + 0x18;

// The assembled code, as 32-bit little-endian words. A test assembles
// bios_rom.S and fails if the result differs from this.
extern const std::uint32_t kBiosRom[];
extern const std::size_t kBiosRomWords;

}  // namespace bd::nds
