#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>

#include "nds/bios_rom.h"

namespace bd::nds {

Result<std::size_t> add_bios_code(const std::filesystem::path& folder) {
  std::string bytes;
  for (std::size_t i = 0; i < kBiosRomWords; ++i)
    for (int b = 0; b < 4; ++b) bytes += static_cast<char>((kBiosRom[i] >> (8 * b)) & 0xFF);
  {
    std::ofstream out(folder / "bios9.bin", std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return Error{ErrorCode::Io, "cannot write " + (folder / "bios9.bin").string()};
  }
  if (!std::filesystem::exists(folder / "manifest.txt"))
    return Error{ErrorCode::Io, "no manifest.txt in " + folder.string()};
  std::ofstream manifest(folder / "manifest.txt", std::ios::binary | std::ios::app);
  char line[100];
  std::snprintf(line, sizeof line, "bios9 load=0x%08" PRIx32 " entry=0x%08" PRIx32 " file=bios9.bin\n",
                kBiosRomAddress, kBiosInterruptVector);
  manifest << line;
  if (!manifest) return Error{ErrorCode::Io, "cannot write " + (folder / "manifest.txt").string()};
  return std::size_t{1};
}

}  // namespace bd::nds
