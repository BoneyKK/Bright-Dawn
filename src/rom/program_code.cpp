#include "rom/program_code.h"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace bd::rom {
namespace {

Status write_file(const std::filesystem::path& path, ByteSpan data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return Error{ErrorCode::Io, "cannot create directory " + path.parent_path().string() + ": " + ec.message()};
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (data.size() > 0)
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) return Error{ErrorCode::Io, "cannot write " + path.string()};
  return {};
}

}  // namespace

Result<std::size_t> write_program_code(const Rom& rom, const std::filesystem::path& folder) {
  std::size_t written = 0;
  auto arm9 = rom.arm9_decompressed();
  if (!arm9) return arm9.error();
  if (auto s = write_file(folder / "arm9.bin", arm9.value()); !s) return s.error();
  if (auto s = write_file(folder / "arm7.bin", rom.arm7()); !s) return s.error();
  written += 2;
  for (const auto& overlay : rom.arm9_overlays()) {
    auto image = rom.load_overlay(overlay);
    if (!image) return image.error();
    char name[32];
    std::snprintf(name, sizeof name, "overlay9_%04u.bin", static_cast<unsigned>(overlay.id));
    if (auto s = write_file(folder / name, image.value().data); !s) return s.error();
    ++written;
  }

  // Where each program file belongs in memory. The code translator needs
  // this: the files themselves do not say where they load or where their
  // entry points are.
  const auto& h = rom.header();
  std::string manifest = "# Bright Dawn code manifest, version 1\n";
  char line[200];
  std::snprintf(line, sizeof line, "arm9 load=0x%08" PRIx32 " entry=0x%08" PRIx32 " file=arm9.bin\n",
                h.arm9.ram_address, h.arm9.entry_address);
  manifest += line;
  std::snprintf(line, sizeof line, "arm7 load=0x%08" PRIx32 " entry=0x%08" PRIx32 " file=arm7.bin\n",
                h.arm7.ram_address, h.arm7.entry_address);
  manifest += line;
  for (const auto& overlay : rom.arm9_overlays()) {
    std::snprintf(line, sizeof line,
                  "overlay9 id=%u load=0x%08" PRIx32 " size=%u bss=%u init_start=0x%08" PRIx32
                  " init_end=0x%08" PRIx32 " file=overlay9_%04u.bin\n",
                  static_cast<unsigned>(overlay.id), overlay.ram_address, static_cast<unsigned>(overlay.ram_size),
                  static_cast<unsigned>(overlay.bss_size), overlay.static_init_start, overlay.static_init_end,
                  static_cast<unsigned>(overlay.id));
    manifest += line;
  }
  const ByteSpan manifest_bytes(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size());
  if (auto s = write_file(folder / "manifest.txt", manifest_bytes); !s) return s.error();
  return written + 1;
}

}  // namespace bd::rom
