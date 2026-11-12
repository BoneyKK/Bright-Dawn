// Writes generated ROM images for the command-line tests:
//   make_test_rom <dark-dawn-like.nds> <other-game.nds>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "rom_builder.h"

namespace {

bool write(const std::filesystem::path& path, const bd_test::Bytes& data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: make_test_rom <dark-dawn-like.nds> <other-game.nds>\n");
    return 2;
  }
  bd_test::RomSpec spec = bd_test::default_spec();
  if (!write(argv[1], bd_test::build_rom(spec).image)) return 1;
  spec.game_code = "ABCE";
  spec.title = "OTHERGAME";
  if (!write(argv[2], bd_test::build_rom(spec).image)) return 1;
  return 0;
}
