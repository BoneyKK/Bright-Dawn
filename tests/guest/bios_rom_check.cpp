// Fails unless the BIOS stand-in embedded in the program (src/nds/bios_rom.cpp)
// is exactly what its source (src/nds/bios_rom.S) assembles to.
//
// usage: bios_rom_check <assembled binary>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "nds/bios_rom.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: bios_rom_check <assembled binary>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() != bd::nds::kBiosRomWords * 4) {
    std::fprintf(stderr, "the source assembles to %zu bytes, but %zu are embedded\n", bytes.size(),
                 bd::nds::kBiosRomWords * 4);
    return 1;
  }
  for (std::size_t i = 0; i < bd::nds::kBiosRomWords; ++i) {
    const std::uint32_t word = static_cast<std::uint32_t>(bytes[i * 4]) | (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
                               (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
                               (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
    if (word != bd::nds::kBiosRom[i]) {
      std::fprintf(stderr, "word %zu: the source assembles to 0x%08" PRIX32 ", but 0x%08" PRIX32 " is embedded\n", i,
                   word, bd::nds::kBiosRom[i]);
      return 1;
    }
  }
  std::printf("the embedded BIOS stand-in matches its source (%zu words)\n", bd::nds::kBiosRomWords);
  return 0;
}
