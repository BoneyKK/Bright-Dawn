// Video memory: nine banks, placed by their control registers.
#include <cstdint>

#include "nds/arm9_io.h"
#include "nds/video_memory.h"
#include "test.h"

namespace {

using bd::nds::VideoMemory;
using Bank = VideoMemory::Bank;

constexpr std::uint8_t kOn = VideoMemory::kEnable;
constexpr std::uint8_t offset(unsigned n) { return static_cast<std::uint8_t>(n << 3); }

// Whether a byte written at `address` lands at `at` in `bank`, and reads back.
bool reaches(VideoMemory& vm, std::uint32_t address, Bank bank, std::size_t at) {
  static std::uint8_t marker = 0;
  marker = static_cast<std::uint8_t>(marker * 7 + 13) | 1;  // Never zero.
  vm.write(address, 1, marker);
  return vm.contents(bank)[at] == marker && vm.read(address, 1) == marker;
}

// Whether nothing is at `address`.
bool empty(VideoMemory& vm, std::uint32_t address) {
  const std::uint64_t before = vm.statistics().reads_with_no_bank;
  return vm.read(address, 1) == 0 && vm.statistics().reads_with_no_bank == before + 1;
}

}  // namespace

TEST(video_memory_has_nine_banks_and_none_is_placed_at_first) {
  VideoMemory vm;
  const std::size_t sizes[] = {131072, 131072, 131072, 131072, 65536, 16384, 16384, 32768, 16384};
  std::size_t total = 0;
  for (unsigned bank = 0; bank < VideoMemory::kBanks; ++bank) {
    CHECK(vm.contents(static_cast<Bank>(bank)).size() == sizes[bank]);
    CHECK(vm.control(static_cast<Bank>(bank)) == 0);
    total += sizes[bank];
  }
  CHECK(total == 656 * 1024);
  for (std::uint32_t address : {0x06000000u, 0x06200000u, 0x06400000u, 0x06600000u, 0x06800000u, 0x06FFFFFFu})
    CHECK(empty(vm, address));
  vm.write(0x06000000, 4, 0x12345678);
  CHECK(vm.statistics().writes_with_no_bank == 1);
}

TEST(video_memory_control_registers_are_write_only_bytes) {
  bd::nds::Arm9Io io;
  VideoMemory vm;
  vm.attach(io);
  const std::uint8_t exist[] = {0x9B, 0x9B, 0x9F, 0x9F, 0x87, 0x9F, 0x9F, 0x83, 0x83};
  for (unsigned bank = 0; bank < VideoMemory::kBanks; ++bank) {
    io.write(VideoMemory::kControlAddresses[bank], 1, 0xFF);
    CHECK(vm.control(static_cast<Bank>(bank)) == exist[bank]);
    CHECK(io.read(VideoMemory::kControlAddresses[bank], 1) == 0);
  }
  // Several at once, as one wider write: A to D.
  io.write(0x04000240, 4, 0x80818283);
  CHECK(vm.control(VideoMemory::kA) == 0x83 && vm.control(VideoMemory::kB) == 0x82);
  CHECK(vm.control(VideoMemory::kC) == 0x81 && vm.control(VideoMemory::kD) == 0x80);
  // The register between G's and H's is not one of these.
  io.write(0x04000247, 1, 0x03);
  CHECK(vm.control(VideoMemory::kG) == 0x9F && vm.control(VideoMemory::kH) == 0x83);
  CHECK(io.unmodelled().size() == 1 && io.unmodelled().count(0x04000247) == 1);
}

TEST(video_memory_as_plain_memory_has_each_bank_at_its_own_place) {
  VideoMemory vm;
  for (unsigned bank = 0; bank < VideoMemory::kBanks; ++bank) vm.set_control(static_cast<Bank>(bank), kOn);
  CHECK(reaches(vm, 0x06800000, VideoMemory::kA, 0) && reaches(vm, 0x0681FFFF, VideoMemory::kA, 0x1FFFF));
  CHECK(reaches(vm, 0x06820000, VideoMemory::kB, 0) && reaches(vm, 0x0683FFFF, VideoMemory::kB, 0x1FFFF));
  CHECK(reaches(vm, 0x06840000, VideoMemory::kC, 0) && reaches(vm, 0x0685FFFF, VideoMemory::kC, 0x1FFFF));
  CHECK(reaches(vm, 0x06860000, VideoMemory::kD, 0) && reaches(vm, 0x0687FFFF, VideoMemory::kD, 0x1FFFF));
  CHECK(reaches(vm, 0x06880000, VideoMemory::kE, 0) && reaches(vm, 0x0688FFFF, VideoMemory::kE, 0xFFFF));
  CHECK(reaches(vm, 0x06890000, VideoMemory::kF, 0) && reaches(vm, 0x06893FFF, VideoMemory::kF, 0x3FFF));
  CHECK(reaches(vm, 0x06894000, VideoMemory::kG, 0) && reaches(vm, 0x06897FFF, VideoMemory::kG, 0x3FFF));
  CHECK(reaches(vm, 0x06898000, VideoMemory::kH, 0) && reaches(vm, 0x0689FFFF, VideoMemory::kH, 0x7FFF));
  CHECK(reaches(vm, 0x068A0000, VideoMemory::kI, 0) && reaches(vm, 0x068A3FFF, VideoMemory::kI, 0x3FFF));
  // Nothing after the last bank, up to where the whole megabyte repeats.
  CHECK(empty(vm, 0x068A4000) && empty(vm, 0x068FFFFF));
  CHECK(reaches(vm, 0x06900010, VideoMemory::kA, 0x10) && reaches(vm, 0x06FA0001, VideoMemory::kI, 1));
  // And nothing in the other ranges.
  CHECK(empty(vm, 0x06000000) && empty(vm, 0x06200000) && empty(vm, 0x06400000) && empty(vm, 0x06600000));

  // Wide accesses are little-endian.
  vm.write(0x06800100, 4, 0x11223344);
  CHECK(vm.contents(VideoMemory::kA)[0x100] == 0x44 && vm.contents(VideoMemory::kA)[0x103] == 0x11);
  CHECK(vm.read(0x06800100, 4) == 0x11223344 && vm.read(0x06800102, 2) == 0x1122);
}

TEST(video_memory_backgrounds_of_the_main_screen) {
  // The four large banks go to any of four places, 128 KiB apart.
  for (Bank bank : {VideoMemory::kA, VideoMemory::kB, VideoMemory::kC, VideoMemory::kD}) {
    for (unsigned n = 0; n < 4; ++n) {
      VideoMemory vm;
      vm.set_control(bank, kOn | offset(n) | 1);
      const std::uint32_t at = 0x06000000 + 0x20000 * n;
      CHECK(reaches(vm, at, bank, 0) && reaches(vm, at + 0x1FFFF, bank, 0x1FFFF));
      CHECK(empty(vm, at ^ 0x20000));
      // The range is 512 KiB and repeats up to the second screen's.
      CHECK(reaches(vm, at + 0x80000 + 5, bank, 5) && reaches(vm, at + 0x180000 + 6, bank, 6));
      CHECK(empty(vm, 0x06800000));
    }
  }
  // E: 64 KiB at the start.
  VideoMemory e;
  e.set_control(VideoMemory::kE, kOn | 1);
  CHECK(reaches(e, 0x06000000, VideoMemory::kE, 0) && reaches(e, 0x0600FFFF, VideoMemory::kE, 0xFFFF));
  CHECK(empty(e, 0x06010000));
  e.set_control(VideoMemory::kE, kOn | offset(3) | 1);  // E has no offset.
  CHECK(e.control(VideoMemory::kE) == 0x81);
  // F and G: 16 KiB at one of four places, and once more 32 KiB further on.
  const std::uint32_t places[4] = {0x06000000, 0x06004000, 0x06010000, 0x06014000};
  for (Bank bank : {VideoMemory::kF, VideoMemory::kG}) {
    for (unsigned n = 0; n < 4; ++n) {
      VideoMemory vm;
      vm.set_control(bank, kOn | offset(n) | 1);
      CHECK(reaches(vm, places[n], bank, 0) && reaches(vm, places[n] + 0x3FFF, bank, 0x3FFF));
      CHECK(reaches(vm, places[n] + 0x8000 + 9, bank, 9));
      CHECK(empty(vm, places[n] + 0x4000) && empty(vm, places[n] + 0xC000));
    }
  }
}

TEST(video_memory_sprites_of_the_main_screen) {
  for (Bank bank : {VideoMemory::kA, VideoMemory::kB}) {
    for (unsigned n = 0; n < 4; ++n) {
      VideoMemory vm;
      vm.set_control(bank, kOn | offset(n) | 2);
      // Only the offset's low bit counts: two places.
      const std::uint32_t at = 0x06400000 + 0x20000 * (n & 1);
      CHECK(reaches(vm, at, bank, 0) && reaches(vm, at + 0x1FFFF, bank, 0x1FFFF));
      CHECK(empty(vm, at ^ 0x20000));
      CHECK(reaches(vm, at + 0x40000 + 3, bank, 3));  // The range is 256 KiB.
    }
  }
  VideoMemory e;
  e.set_control(VideoMemory::kE, kOn | 2);
  CHECK(reaches(e, 0x06400000, VideoMemory::kE, 0) && reaches(e, 0x0640FFFF, VideoMemory::kE, 0xFFFF));
  CHECK(empty(e, 0x06410000));
  VideoMemory g;
  g.set_control(VideoMemory::kG, kOn | offset(3) | 2);
  CHECK(reaches(g, 0x06414000, VideoMemory::kG, 0) && reaches(g, 0x0641C001, VideoMemory::kG, 1));
  CHECK(empty(g, 0x06400000));
}

TEST(video_memory_of_the_second_screen) {
  VideoMemory c;
  c.set_control(VideoMemory::kC, kOn | 4);
  CHECK(reaches(c, 0x06200000, VideoMemory::kC, 0) && reaches(c, 0x0621FFFF, VideoMemory::kC, 0x1FFFF));
  CHECK(reaches(c, 0x06220004, VideoMemory::kC, 4) && reaches(c, 0x063E0005, VideoMemory::kC, 5));  // 128 KiB, repeating.
  CHECK(empty(c, 0x06000000) && empty(c, 0x06600000));

  // H: 32 KiB at the start and again 64 KiB on. I: 16 KiB after each of
  // those, twice in a row.
  VideoMemory hi;
  hi.set_control(VideoMemory::kH, kOn | 1);
  hi.set_control(VideoMemory::kI, kOn | 1);
  CHECK(reaches(hi, 0x06200000, VideoMemory::kH, 0) && reaches(hi, 0x06207FFF, VideoMemory::kH, 0x7FFF));
  CHECK(reaches(hi, 0x06210002, VideoMemory::kH, 2));
  CHECK(reaches(hi, 0x06208000, VideoMemory::kI, 0) && reaches(hi, 0x0620C001, VideoMemory::kI, 1));
  CHECK(reaches(hi, 0x06218002, VideoMemory::kI, 2) && reaches(hi, 0x0621FFFF, VideoMemory::kI, 0x3FFF));

  VideoMemory d;
  d.set_control(VideoMemory::kD, kOn | 4);
  CHECK(reaches(d, 0x06600000, VideoMemory::kD, 0) && reaches(d, 0x0661FFFF, VideoMemory::kD, 0x1FFFF));
  CHECK(reaches(d, 0x06620007, VideoMemory::kD, 7));
  CHECK(empty(d, 0x06200000));

  // I as sprite memory: its 16 KiB, over and over.
  VideoMemory i;
  i.set_control(VideoMemory::kI, kOn | 2);
  for (std::uint32_t at = 0x06600000; at < 0x06620000; at += 0x4000) CHECK(reaches(i, at + 8, VideoMemory::kI, 8));
}

TEST(video_memory_banks_at_the_same_place_are_written_together_and_read_ored) {
  VideoMemory vm;
  // Different contents, put there while each bank is plain memory.
  vm.set_control(VideoMemory::kA, kOn);
  vm.set_control(VideoMemory::kB, kOn);
  vm.set_control(VideoMemory::kE, kOn);
  vm.write(0x06800000, 4, 0x000000F0);
  vm.write(0x06820000, 4, 0x00000F01);
  vm.write(0x06880000, 4, 0x00A00000);
  vm.set_control(VideoMemory::kA, kOn | 1);
  vm.set_control(VideoMemory::kB, kOn | 1);
  vm.set_control(VideoMemory::kE, kOn | 1);
  CHECK(vm.read(0x06000000, 4) == 0x00A00FF1);
  CHECK(vm.read(0x06010000, 4) == 0);  // Past E's end: A and B only, both zero there.
  vm.write(0x06000000, 2, 0x1234);
  CHECK(vm.contents(VideoMemory::kA)[0] == 0x34 && vm.contents(VideoMemory::kB)[1] == 0x12);
  CHECK(vm.contents(VideoMemory::kE)[0] == 0x34 && vm.contents(VideoMemory::kE)[2] == 0xA0);
  CHECK(vm.statistics().reads_with_no_bank == 0 && vm.statistics().writes_with_no_bank == 0);
}

TEST(video_memory_keeps_a_bank_s_contents_wherever_it_goes) {
  // What a game does with a texture: write it while the bank is plain memory,
  // hand the bank to the 3D engine, later take it back.
  VideoMemory vm;
  vm.set_control(VideoMemory::kA, kOn);
  vm.write(0x06801000, 4, 0xCAFEF00D);
  vm.set_control(VideoMemory::kA, kOn | 3);  // Textures: out of the processor's reach.
  CHECK(empty(vm, 0x06801000) && empty(vm, 0x06001000) && empty(vm, 0x06401000));
  vm.write(0x06801000, 4, 0);
  CHECK(vm.contents(VideoMemory::kA)[0x1000] == 0x0D && vm.contents(VideoMemory::kA)[0x1003] == 0xCA);
  vm.set_control(VideoMemory::kA, 0x03);  // Switched off.
  CHECK(empty(vm, 0x06801000));
  vm.set_control(VideoMemory::kA, kOn);
  CHECK(vm.read(0x06801000, 4) == 0xCAFEF00D);
}

TEST(video_memory_uses_that_give_a_bank_no_address) {
  struct Case {
    Bank bank;
    std::uint8_t use;
  };
  // Textures and their palettes, extra palettes, the second processor's, and
  // settings that mean nothing.
  const Case cases[] = {{VideoMemory::kA, 3}, {VideoMemory::kB, 3}, {VideoMemory::kC, 2}, {VideoMemory::kC, 3},
                        {VideoMemory::kC, 5}, {VideoMemory::kD, 2}, {VideoMemory::kD, 3}, {VideoMemory::kD, 7},
                        {VideoMemory::kE, 3}, {VideoMemory::kE, 4}, {VideoMemory::kE, 5}, {VideoMemory::kF, 3},
                        {VideoMemory::kF, 4}, {VideoMemory::kF, 5}, {VideoMemory::kG, 6}, {VideoMemory::kH, 2},
                        {VideoMemory::kH, 3}, {VideoMemory::kI, 3}};
  for (const Case& c : cases) {
    VideoMemory vm;
    vm.set_control(c.bank, static_cast<std::uint8_t>(kOn | c.use));
    for (std::uint32_t at = 0x06000000; at < 0x06900000; at += 0x4000) {
      if (!empty(vm, at)) CHECK(false);
    }
  }
}

TEST(video_memory_textures_for_the_3d_engine) {
  VideoMemory vm;
  // Nothing is there until a bank is handed over.
  CHECK(vm.texture(0) == -1 && vm.texture(0x7FFFF) == -1);
  // Banks are filled as plain memory and then handed over. Each of A to D
  // can be any of the four slots of 128 KiB.
  vm.set_control(VideoMemory::kA, kOn);
  vm.set_control(VideoMemory::kC, kOn);
  vm.write(0x06800000, 1, 0x11);  // A's first byte and its last.
  vm.write(0x0681FFFF, 1, 0x12);
  vm.write(0x06840000, 1, 0x31);  // C's.
  vm.write(0x0685FFFF, 1, 0x32);
  CHECK(vm.texture(0) == -1);
  vm.set_control(VideoMemory::kA, kOn | 3 | offset(2));
  vm.set_control(VideoMemory::kC, kOn | 3 | offset(0));
  CHECK(vm.texture(0x00000) == 0x31 && vm.texture(0x1FFFF) == 0x32);
  CHECK(vm.texture(0x40000) == 0x11 && vm.texture(0x5FFFF) == 0x12);
  CHECK(vm.texture(0x20000) == -1 && vm.texture(0x60000) == -1);
  // The main processor cannot reach them there.
  CHECK(empty(vm, 0x06800000) && empty(vm, 0x06840000));
  // Two banks at one slot read ORed together.
  vm.set_control(VideoMemory::kA, kOn | 3 | offset(0));
  CHECK(vm.texture(0x00000) == 0x31 && vm.texture(0x1FFFF) == 0x32);
  CHECK(vm.texture(0x40000) == -1);
  vm.set_control(VideoMemory::kC, kOn);
  vm.write(0x06840000, 1, 0x0E);
  vm.set_control(VideoMemory::kC, kOn | 3);
  CHECK(vm.texture(0x00000) == 0x1F);
  // Switched off, a bank is nowhere.
  vm.set_control(VideoMemory::kA, 3);
  vm.set_control(VideoMemory::kC, 3);
  CHECK(vm.texture(0) == -1);
  // B and D as well.
  vm.set_control(VideoMemory::kB, kOn);
  vm.set_control(VideoMemory::kD, kOn);
  vm.write(0x06820004, 1, 0x21);
  vm.write(0x06860008, 1, 0x41);
  vm.set_control(VideoMemory::kB, kOn | 3 | offset(3));
  vm.set_control(VideoMemory::kD, kOn | 3 | offset(1));
  CHECK(vm.texture(0x60004) == 0x21 && vm.texture(0x20008) == 0x41);
  // The texture memory is 512 KiB and repeats.
  CHECK(vm.texture(0x80000 + 0x60004) == 0x21);
}

TEST(video_memory_texture_palettes_for_the_3d_engine) {
  VideoMemory vm;
  CHECK(vm.texture_palette(0) == -1);
  // Bank E fills the first four of the six slots of 16 KiB.
  vm.set_control(VideoMemory::kE, kOn);
  vm.write(0x06880000, 1, 0x51);
  vm.write(0x0688FFFF, 1, 0x52);
  vm.set_control(VideoMemory::kE, kOn | 3);
  CHECK(vm.texture_palette(0x00000) == 0x51 && vm.texture_palette(0x0FFFF) == 0x52);
  CHECK(vm.texture_palette(0x10000) == -1 && vm.texture_palette(0x14000) == -1);
  // Banks F and G fill slot 0, 1, 4 or 5, by their offset.
  vm.set_control(VideoMemory::kF, kOn);
  vm.set_control(VideoMemory::kG, kOn);
  vm.write(0x06890000, 1, 0x61);
  vm.write(0x06894000, 1, 0x71);
  vm.set_control(VideoMemory::kF, kOn | 3 | offset(2));
  vm.set_control(VideoMemory::kG, kOn | 3 | offset(3));
  CHECK(vm.texture_palette(0x10000) == 0x61 && vm.texture_palette(0x14000) == 0x71);
  vm.set_control(VideoMemory::kE, 0);
  vm.set_control(VideoMemory::kF, kOn | 3 | offset(0));
  vm.set_control(VideoMemory::kG, kOn | 3 | offset(1));
  CHECK(vm.texture_palette(0x00000) == 0x61 && vm.texture_palette(0x04000) == 0x71);
  CHECK(vm.texture_palette(0x08000) == -1 && vm.texture_palette(0x10000) == -1);
  // There is nothing above the six slots.
  CHECK(vm.texture_palette(0x18000) == -1 && vm.texture_palette(0xFFFFFFFF) == -1);
}
