#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "nds/program.h"
#include "rom_builder.h"
#include "test.h"

namespace {

// A directory of its own for one test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("bd_program_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
             std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }
  void write(const std::string& name, const std::string& content) const {
    std::ofstream out(path_ / name, std::ios::binary);
    out << content;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(program_from_directory_reads_the_manifest) {
  TempDir dir;
  dir.write("arm9.bin", std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
  dir.write("arm7.bin", "ignored");
  dir.write("overlay9_0000.bin", "abcd");
  dir.write("overlay9_0001.bin", "efghij");
  dir.write("manifest.txt",
            "# Bright Dawn code manifest, version 1\n"
            "arm9 load=0x02000000 entry=0x02000800 file=arm9.bin\n"
            "arm7 load=0x02380000 entry=0x02380000 file=arm7.bin\n"
            "overlay9 id=0 load=0x02100000 size=4 bss=16 init_start=0x02100000 init_end=0x02100004 "
            "file=overlay9_0000.bin\r\n"
            "\n"
            "overlay9 id=1 load=0x02100000 size=6 bss=0 init_start=0 init_end=0 file=overlay9_0001.bin\n");
  auto program = bd::nds::program_from_directory(dir.path());
  CHECK(program.ok());
  if (!program) return;
  const bd::nds::Program& p = program.value();
  CHECK(p.arm9.size() == 8 && p.arm9[7] == 8);
  CHECK(p.arm9_load_address == 0x02000000);
  CHECK(p.arm9_entry == 0x02000800);
  CHECK(p.header.empty());
  CHECK(p.overlays.size() == 2);
  if (p.overlays.size() != 2) return;
  CHECK(p.overlays[0].id == 0 && p.overlays[0].load_address == 0x02100000);
  CHECK(p.overlays[0].bss_size == 16);
  CHECK(p.overlays[0].static_init_start == 0x02100000 && p.overlays[0].static_init_end == 0x02100004);
  CHECK(p.overlays[0].image.size() == 4 && p.overlays[0].image[0] == 'a');
  CHECK(p.overlays[1].id == 1 && p.overlays[1].image.size() == 6);
}

TEST(program_from_directory_reports_what_is_wrong) {
  {
    TempDir dir;  // No manifest at all.
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().code == bd::ErrorCode::Io);
  }
  {
    TempDir dir;  // No main program.
    dir.write("manifest.txt", "# nothing\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().message.find("no 'arm9' line") != std::string::npos);
  }
  {
    TempDir dir;  // A field that is not a number.
    dir.write("arm9.bin", "x");
    dir.write("manifest.txt", "arm9 load=here entry=0x02000800 file=arm9.bin\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().message.find("'load'") != std::string::npos);
  }
  {
    TempDir dir;  // A word that is not key=value.
    dir.write("arm9.bin", "x");
    dir.write("manifest.txt", "arm9 load=0 entry=0 file=arm9.bin stray\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().message.find("key=value") != std::string::npos);
  }
  {
    TempDir dir;  // A file that is not there.
    dir.write("manifest.txt", "arm9 load=0 entry=0 file=arm9.bin\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().code == bd::ErrorCode::Io);
  }
  {
    TempDir dir;  // An overlay whose file is not the size the manifest gives.
    dir.write("arm9.bin", "x");
    dir.write("overlay9_0000.bin", "abc");
    dir.write("manifest.txt",
              "arm9 load=0 entry=0 file=arm9.bin\n"
              "overlay9 id=0 load=0 size=4 bss=0 init_start=0 init_end=0 file=overlay9_0000.bin\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().message.find("manifest says 4") != std::string::npos);
  }
  {
    TempDir dir;  // Two main programs.
    dir.write("arm9.bin", "x");
    dir.write("manifest.txt", "arm9 load=0 entry=0 file=arm9.bin\narm9 load=0 entry=0 file=arm9.bin\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
  }
}

TEST(program_from_directory_only_reads_files_in_the_directory) {
  TempDir dir;
  dir.write("arm9.bin", "x");
  for (const char* name : {"../arm9.bin", "sub/arm9.bin", "..", "C:arm9.bin", "sub\\arm9.bin", "/etc/hostname"}) {
    dir.write("manifest.txt", std::string("arm9 load=0 entry=0 file=") + name + "\n");
    auto program = bd::nds::program_from_directory(dir.path());
    CHECK(!program.ok());
    if (!program) CHECK(program.error().code == bd::ErrorCode::Corrupt);
  }
}

TEST(program_from_rom_takes_code_header_and_overlays) {
  bd_test::RomSpec spec;
  bd_test::OverlaySpec raw;
  raw.data = bd_test::Bytes(64, 0x11);
  bd_test::OverlaySpec packed;
  packed.data = bd_test::Bytes(300, 0x22);
  packed.storage = bd_test::OverlayStorage::Lz40;
  packed.ram_address = 0x02180000;
  spec.overlays = {raw, packed};
  const bd_test::BuiltRom built = bd_test::build_rom(spec);
  auto rom = bd::rom::Rom::from_bytes(built.image);
  CHECK(rom.ok());
  if (!rom) return;
  auto program = bd::nds::program_from_rom(rom.value());
  CHECK(program.ok());
  if (!program) return;
  const bd::nds::Program& p = program.value();
  CHECK(p.arm9 == built.arm9_plain);  // Decompressed.
  CHECK(p.arm9_load_address == rom.value().header().arm9.ram_address);
  CHECK(p.arm9_entry == rom.value().header().arm9.entry_address);
  CHECK(p.header.size() == 0x170);
  CHECK(std::equal(p.header.begin(), p.header.end(), built.image.begin()));
  CHECK(p.overlays.size() == 2);
  if (p.overlays.size() != 2) return;
  CHECK(p.overlays[0].image == raw.data && p.overlays[0].load_address == 0x02100000);
  CHECK(p.overlays[1].image == packed.data && p.overlays[1].load_address == 0x02180000);
}
