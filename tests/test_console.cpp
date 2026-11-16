// The console runs small hand-assembled programs. The words were produced
// with the same assembler as the guest tests; the source is beside each.
#include <cstdint>
#include <string>
#include <vector>

#include "nds/console.h"
#include "rom_builder.h"
#include "test.h"

namespace {

const TranslatedEntry kNoTranslations[] = {{0, nullptr, -1, nullptr, 0}};

bd::nds::Program program_of(const std::vector<std::uint32_t>& words) {
  bd::nds::Program program;
  program.arm9_load_address = 0x02000000;
  program.arm9_entry = 0x02000000;
  program.arm9.resize(words.size() * 4);
  for (std::size_t i = 0; i < words.size(); ++i) {
    for (int b = 0; b < 4; ++b) program.arm9[i * 4 + b] = static_cast<std::uint8_t>(words[i] >> (8 * b));
  }
  return program;
}

// What a game's start-up code does first, in miniature:
//   02000000  mov  r12, #0x04000000
//   02000004  str  r12, [r12, #0x208]      a hardware register
//   02000008  ldr  r0, =0x027E000A
//   0200000c  mcr  p15, 0, r0, c9, c1, 0   move the data memory
//   02000010  mov  r0, #0x13
//   02000014  msr  cpsr_c, r0              Supervisor mode
//   02000018  ldr  sp, =0x027E3FC0
//   0200001c  mov  r0, #0x12
//   02000020  msr  cpsr_c, r0              IRQ mode
//   02000024  ldr  sp, =0x027E3F80
//   02000028  mov  r0, #0x1F
//   0200002c  msr  cpsr_c, r0              System mode
//   02000030  ldr  sp, =0x027E3000
//   02000034  ldr  r1, =0x12345678
//   02000038  push {r1}                    into the data memory
//   0200003c  bl   helper
//   02000040  ldrh r5, [r12, #6]
//   02000044  b    .
//   02000048  helper: add r4, r1, #1
//   0200004c          bx  lr
//   02000050  the five constants
const std::vector<std::uint32_t> kStartUp = {
    0xE3A0C301, 0xE58CC208, 0xE59F0040, 0xEE090F11, 0xE3A00013, 0xE121F000, 0xE59FD034,
    0xE3A00012, 0xE121F000, 0xE59FD02C, 0xE3A0001F, 0xE121F000, 0xE59FD024, 0xE59F1024,
    0xE52D1004, 0xEB000001, 0xE1DC50B6, 0xEAFFFFFE, 0xE2814001, 0xE12FFF1E, 0x027E000A,
    0x027E3FC0, 0x027E3F80, 0x027E3000, 0x12345678};

}  // namespace

TEST(console_direct_boot_sets_the_firmware_state) {
  const bd::nds::Program program = program_of(kStartUp);
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  ds::Cpu& c = console.machine().cpu();
  CHECK(ds::get_cpsr(c) == 0xDF);  // System mode, interrupts masked.
  CHECK(c.r[13] == 0x03002F7C);
  CHECK(c.r[12] == 0x02000000);
  CHECK(c.bank_r13[2] == 0x03003F80);  // IRQ mode's stack.
  CHECK(c.bank_r13[3] == 0x03003FC0);  // Supervisor mode's stack.
  CHECK(console.system_control().control() == 0x0005707D);
  CHECK(console.bus().read32(0x02000000) == 0xE3A0C301);  // The program is in memory.
  CHECK(console.bus().read32(0x027FF800) == 0x00001FC2);
  CHECK(console.bus().read16(0x027FFC40) == 0x0001);
  CHECK(console.bus().read8(0x04000300) == 0x01);
  CHECK(console.io().unmodelled().at(0x04000300).writes == 0);  // Set by the firmware, not the program.
}

TEST(console_direct_boot_leaves_the_user_settings_in_memory) {
  const bd::nds::Program program = program_of(kStartUp);
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  // The defaults: English, and a network address that is not all zeros.
  CHECK(console.bus().read16(0x027FFC80) == 5);
  CHECK((console.bus().read16(0x027FFCE4) & 7) == 1);
  std::uint32_t address_bytes = 0;
  for (std::uint32_t i = 0; i < 6; ++i) address_bytes |= console.bus().read8(0x02FFFCF4 + i);
  CHECK(address_bytes != 0);

  bd::nds::UserSettings settings;
  settings.language = bd::nds::UserSettings::Language::French;
  settings.nickname = u"Isaac";
  bd::nds::Console french(program, kNoTranslations);
  french.set_user_settings(settings);
  CHECK(french.direct_boot().ok());
  CHECK((french.bus().read16(0x027FFCE4) & 7) == 2);
  CHECK(french.bus().read16(0x027FFC86) == 'I' && french.bus().read16(0x027FFC9A) == 5);

  settings.nickname = u"Far too long a name";
  bd::nds::Console invalid(program, kNoTranslations);
  invalid.set_user_settings(settings);
  CHECK(!invalid.direct_boot().ok());
}

TEST(console_starts_with_the_sound_on_and_silent) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  // As the second processor's sound program leaves the hardware: enabled,
  // master volume full, outputs from the mixer.
  CHECK(console.sound_hardware().read(bd::nds::SoundHardware::kControl, 2) == 0x807F);
  // A hundredth of a second (335,140 cycles, one per instruction): 327
  // output samples of 1,024 cycles, none with anything in it, and the sound
  // program has not had its first beat.
  CHECK(console.run(335140).reason == bd::nds::StopReason::InstructionLimit);
  console.timeline().refresh();
  CHECK(console.sound_hardware().statistics().samples == 327);
  CHECK(console.sound_hardware().statistics().samples_with_sound == 0);
  CHECK(console.sound_driver().statistics().beats == 0);
}

TEST(console_runs_dma_at_the_blanks_of_the_display) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  constexpr std::uint32_t kTable = 0x02100000, kPlace = 0x02200000, kOther = 0x02200100, kThird = 0x02200200;
  for (std::uint32_t i = 0; i < 600; ++i) console.bus().write16(kTable + 2 * i, static_cast<std::uint16_t>(i + 1));
  constexpr std::uint32_t kEnable = 0x80000000, kRepeat = 1u << 25, kFixedDestination = 2u << 21;
  constexpr std::uint32_t kAtVBlank = 1u << 27, kAtHBlank = 2u << 27;
  const auto run_to = [&](std::uint64_t time) {
    const std::uint64_t now = console.timeline().now();
    CHECK(console.run(time - now).reason == bd::nds::StopReason::InstructionLimit);
    console.timeline().refresh();
    CHECK(console.timeline().now() == time);
  };
  constexpr std::uint64_t kLine = 2130, kHBlank = 1536, kFrame = 263 * kLine;

  // Channel 0: at every horizontal blank, the next 16-bit value of the table
  // to one fixed place. Channel 1: once, three values. Channel 2: at every
  // vertical blank, one value.
  console.io().write(0x040000B0, 4, kTable);
  console.io().write(0x040000B4, 4, kPlace);
  console.io().write(0x040000B8, 4, kEnable | kAtHBlank | kRepeat | kFixedDestination | 1);
  CHECK(console.video_timing().watching_horizontal_blank());
  console.io().write(0x040000BC, 4, kTable + 0x200);
  console.io().write(0x040000C0, 4, kOther);
  console.io().write(0x040000C4, 4, kEnable | kAtHBlank | 3);
  console.io().write(0x040000C8, 4, kTable + 0x400);
  console.io().write(0x040000CC, 4, kThird);
  console.io().write(0x040000D0, 4, kEnable | kAtVBlank | kRepeat | kFixedDestination | 1);
  CHECK(console.dma().statistics().transfers == 0);

  run_to(kHBlank - 1);
  CHECK(console.dma().statistics().transfers == 0);
  run_to(kHBlank);
  CHECK(console.dma().statistics().transfers == 2);
  CHECK(console.bus().read16(kPlace) == 1);
  CHECK(console.bus().read16(kOther) == 0x101 && console.bus().read16(kOther + 4) == 0x103);
  // Channel 1 is finished; channel 0 goes on, a line at a time.
  CHECK((console.io().read(0x040000C4, 4) & kEnable) == 0);
  CHECK((console.io().read(0x040000B8, 4) & kEnable) != 0);
  run_to(kLine + kHBlank - 1);
  CHECK(console.bus().read16(kPlace) == 1);
  run_to(kLine + kHBlank);
  CHECK(console.bus().read16(kPlace) == 2);
  CHECK(console.bus().read16(kOther) == 0x101);

  // Line 191 is the last with a horizontal blank that counts. The vertical
  // blank begins with line 192.
  run_to(191 * kLine + kHBlank);
  CHECK(console.bus().read16(kPlace) == 192);
  CHECK(console.bus().read16(kThird) == 0);
  run_to(192 * kLine - 1);
  CHECK(console.bus().read16(kThird) == 0);
  run_to(192 * kLine);
  CHECK(console.bus().read16(kThird) == 0x201);
  run_to(kFrame + kHBlank - 1);
  CHECK(console.bus().read16(kPlace) == 192);
  CHECK(console.dma().statistics().transfers == 192 + 1 + 1);
  run_to(kFrame + kHBlank);
  CHECK(console.bus().read16(kPlace) == 193);

  // With channel 0 switched off nothing waits for the horizontal blank, and
  // after the next one it is no longer watched for.
  console.io().write(0x040000B8, 4, 0);
  run_to(kFrame + kLine + kHBlank);
  CHECK(console.bus().read16(kPlace) == 193);
  CHECK(!console.video_timing().watching_horizontal_blank());
  run_to(2 * kFrame);
  CHECK(console.bus().read16(kThird) == 0x202);
  CHECK(console.dma().statistics().transfers == 193 + 1 + 2);
}

TEST(console_makes_the_processor_wait_while_the_3d_queue_is_full) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  // After "swap buffers" the engine stands still until the vertical blank,
  // and what is sent meanwhile queues up: 256 entries and the four the
  // hardware has taken ahead.
  console.io().write(0x04000540, 4, 0);
  for (int i = 0; i < 260; ++i) console.io().write(0x04000440, 4, 1);
  CHECK(console.timeline().now() == 0);
  CHECK(!console.geometry().room());
  // The next write does not come back until there is room: at the vertical
  // blank, which begins with line 192.
  console.io().write(0x04000440, 4, 2);
  CHECK(console.timeline().now() == 192 * 2130);
  CHECK(console.geometry().statistics().waits_for_room == 1);
  CHECK(console.geometry().statistics().swaps == 1);
  CHECK(console.geometry().matrix_mode() == 2);
  CHECK(console.geometry().queued() == 0);
}

TEST(console_feeds_the_3d_queue_by_dma) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  constexpr std::uint32_t kList = 0x02100000;
  constexpr std::uint32_t kToQueue = 0x80000000 | (7u << 27) | (1u << 26) | (2u << 21);  // Words, to one place.
  // "Choose the matrix": 2.
  console.bus().write32(kList, 0x00000010);
  console.bus().write32(kList + 4, 2);
  console.io().write(0x040000B0, 4, kList);
  console.io().write(0x040000B4, 4, 0x04000400);
  console.io().write(0x040000B8, 4, kToQueue | 2);
  // The queue is less than half full, so the transfer runs at once.
  CHECK(console.geometry().matrix_mode() == 2);
  CHECK(console.dma().statistics().transfers == 1);
  CHECK((console.io().read(0x040000B8, 4) & 0x80000000) == 0);

  // With the queue half full or more, a transfer waits for it to drain.
  console.io().write(0x04000540, 4, 0);
  for (int i = 0; i < 132; ++i) console.io().write(0x04000440, 4, 1);
  CHECK(!console.geometry().less_than_half_full());
  console.bus().write32(kList + 4, 3);
  console.io().write(0x040000B8, 4, kToQueue | 2);
  CHECK(console.dma().statistics().transfers == 1);
  CHECK(console.run(192 * 2130 + 10).reason == bd::nds::StopReason::InstructionLimit);
  CHECK(console.dma().statistics().transfers == 2);
  CHECK(console.geometry().matrix_mode() == 3);
}

TEST(console_draws_what_the_3d_engine_is_sent) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  bd::nds::Arm9Io& io = console.io();
  io.write(0x04000354, 2, 0x7FFF);       // Clear depth: the far side.
  io.write(0x040004A4, 4, 0x001F00C0);   // Polygon attributes: both sides, solid.
  io.write(0x04000500, 4, 0);            // Begin: triangles.
  io.write(0x04000480, 4, 0x001F);       // Colour: red.
  // The lower left half of the screen: the corners of the view are at one
  // unit, 0x040 in a vertex of ten bits a coordinate.
  io.write(0x04000490, 4, 0x3C0u | 0x040u << 10);  // (-1, 1): top left.
  io.write(0x04000490, 4, 0x3C0u | 0x3C0u << 10);  // (-1, -1): bottom left.
  io.write(0x04000490, 4, 0x040u | 0x3C0u << 10);  // (1, -1): bottom right.
  CHECK(io.read(0x04000604, 4) == 0x00030001);
  io.write(0x04000540, 4, 0);            // Swap buffers.
  // One instruction is one cycle here: run to past the vertical blank.
  CHECK(console.run(192 * 2130 + 10).reason == bd::nds::StopReason::InstructionLimit);
  CHECK(console.geometry().statistics().swaps == 1);
  const bd::nds::PolygonRenderer::Image& image =
      console.polygon_renderer().draw(console.geometry().finished(), console.bus().video_memory());
  const auto at = [&](int x, int y) { return image[static_cast<std::size_t>(y * 256 + x)]; };
  const std::uint32_t red = 63u | 63u << 18;
  CHECK(at(0, 0) == red && at(0, 191) == red && at(100, 150) == red);
  CHECK(at(255, 0) == 0 && at(150, 50) == 0);

  // The drawing half's control register tells when a frame was too full,
  // until the program writes the flag back.
  CHECK(io.read(0x04000060, 2) == 0);
  io.write(0x04000500, 4, 0);
  for (int i = 0; i < 2049 * 3; ++i) io.write(0x04000490, 4, i % 3 == 0 ? 0u : i % 3 == 1 ? 0x020u : 0x020u << 10);
  CHECK(io.read(0x04000604, 4) == 0x18000800);
  CHECK(io.read(0x04000060, 2) == 0x2000);
  io.write(0x04000060, 2, 0x2000);
  CHECK(io.read(0x04000060, 2) == 0);
}

TEST(console_has_buttons_that_are_not_held) {
  // A program that reads the button register and stops:
  //   02000000  mov  r0, #0x04000000
  //   02000004  add  r0, r0, #0x130
  //   02000008  ldrh r4, [r0]
  //   0200000c  b    .
  const bd::nds::Program program = program_of({0xE3A00301, 0xE2800E13, 0xE1D040B0, 0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  CHECK(console.bus().read16(0x027FFFA8) == 0x2C00);  // X, Y and the lid, from the second processor.
  CHECK(console.run(100).reason == bd::nds::StopReason::InstructionLimit);
  CHECK(console.machine().cpu().r[4] == 0x03FF);
  CHECK(console.io().unmodelled().empty());

  // Held buttons show in both places, at once.
  bd::nds::Console held(program, kNoTranslations);
  CHECK(held.direct_boot().ok());
  held.keypad().set_held((1u << static_cast<unsigned>(bd::nds::Button::Start)) |
                         (1u << static_cast<unsigned>(bd::nds::Button::X)));
  CHECK(held.bus().read16(0x027FFFA8) == 0x2800);
  CHECK(held.run(100).reason == bd::nds::StopReason::InstructionLimit);
  CHECK(held.machine().cpu().r[4] == 0x03F7);
  held.keypad().set_lid_closed(true);
  CHECK(held.bus().read16(0x027FFFA8) == 0xA800);

  // Buttons held before the program starts are there when it starts.
  bd::nds::Console early(program, kNoTranslations);
  early.keypad().set_held(1u << static_cast<unsigned>(bd::nds::Button::Y));
  CHECK(early.direct_boot().ok());
  CHECK(early.bus().read16(0x027FFFA8) == 0x2400);
}

TEST(console_direct_boot_rejects_a_program_that_does_not_fit) {
  bd::nds::Program program = program_of(kStartUp);
  program.arm9_load_address = 0x023FFFF0;
  bd::nds::Console console(program, kNoTranslations);
  CHECK(!console.direct_boot().ok());
  program.arm9_load_address = 0x01FF8000;
  bd::nds::Console other(program, kNoTranslations);
  CHECK(!other.direct_boot().ok());
}

TEST(console_direct_boot_copies_the_header_and_clears_the_compression_field) {
  bd_test::RomSpec spec;  // Its ARM9 program is compressed and has start-up parameters.
  const bd_test::BuiltRom built = bd_test::build_rom(spec);
  auto rom = bd::rom::Rom::from_bytes(built.image);
  CHECK(rom.ok());
  if (!rom) return;
  CHECK(rom.value().module_params().has_value());
  if (!rom.value().module_params()) return;
  CHECK(rom.value().module_params()->compressed_static_end != 0);
  auto program = bd::nds::program_from_rom(rom.value());
  CHECK(program.ok());
  if (!program) return;
  bd::nds::Console console(program.value(), kNoTranslations);
  CHECK(console.direct_boot().ok());

  const auto params = bd::rom::find_module_params(program.value().arm9);
  CHECK(params.has_value());
  if (!params) return;
  const std::uint32_t base = program.value().arm9_load_address + params->offset;
  CHECK(console.bus().read32(base + 0x14) == 0);                     // "Not compressed."
  CHECK(console.bus().read32(base + 0x18) == params->sdk_version);   // Its neighbours are untouched.
  CHECK(console.bus().read32(base + 0x10) == params->static_bss_end);
  // The header copy, byte for byte.
  bool same = true;
  for (std::uint32_t i = 0; i < 0x170; ++i) same = same && console.bus().read8(0x027FFE00 + i) == built.image[i];
  CHECK(same);
}

TEST(console_runs_start_up_code) {
  const bd::nds::Program program = program_of(kStartUp);
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::InstructionLimit);
  CHECK(result.address == 0x02000044);  // In its final loop.

  ds::Cpu& c = console.machine().cpu();
  CHECK((ds::get_cpsr(c) & 0x1F) == 0x1F);
  CHECK(c.r[13] == 0x027E2FFC);
  CHECK(c.r[4] == 0x12345679);  // The helper ran and returned.
  CHECK(c.bank_r13[2] == 0x027E3F80);
  CHECK(c.bank_r13[3] == 0x027E3FC0);
  // The data memory moved, and the push went into it.
  CHECK(console.system_control().read(9, 1, 0) == 0x027E000A);
  CHECK(console.bus().read32(0x027E2FFC) == 0x12345678);
  CHECK(console.bus().read32(0x023E2FFC) == 0);  // Not into the main memory under it.
  // The write to the interrupt master switch reached the interrupt
  // controller (bit 0 of what was written is clear), and the line counter
  // was read: after 17 instructions the display is still on its first line.
  CHECK(!console.interrupts().master());
  CHECK(console.io().unmodelled().empty());
  CHECK(c.r[5] == 0);
  CHECK(console.software_interrupts().empty());
  CHECK(console.machine().statistics().interpreted_instructions == 1000);
}

TEST(console_performs_bios_calls) {
  //   02000000  mov r0, #16
  //   02000004  svc 0x0D0000     the square root
  //   02000008  add r0, r0, #1
  //   0200000c  b   .
  const bd::nds::Program program = program_of({0xE3A00010, 0xEF0D0000, 0xE2800001, 0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::InstructionLimit);
  CHECK(result.address == 0x0200000C);
  CHECK(console.software_interrupts().at(0x0D) == 1);
  CHECK(console.machine().cpu().r[0] == 5);
}

TEST(console_stops_at_a_bios_call_that_is_not_implemented) {
  //   02000000  mov r0, #1
  //   02000004  svc 0x2A0000     no such call
  //   02000008  mov r0, #2
  //   0200000c  b   .
  const bd::nds::Program program = program_of({0xE3A00001, 0xEF2A0000, 0xE3A00002, 0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::Unhandled);
  CHECK(result.address == 0x02000004);
  CHECK(result.detail.find("BIOS call") != std::string::npos);
  CHECK(console.software_interrupts().at(0x2A) == 1);
  CHECK(console.machine().cpu().r[0] == 1);  // Nothing after it ran.
  CHECK(console.machine().statistics().interpreted_instructions == 1);
}

TEST(console_reports_a_program_that_returns) {
  //   02000000  mov r0, #7
  //   02000004  bx  lr
  const bd::nds::Program program = program_of({0xE3A00007, 0xE12FFF1E});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::Returned);
  CHECK(console.machine().cpu().r[0] == 7);
}

TEST(console_stops_at_an_undefined_instruction) {
  //   02000000  mov r0, #7
  //   02000004  an undefined instruction
  const bd::nds::Program program = program_of({0xE3A00007, 0xE7F000F0});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::Unhandled);
  CHECK(result.address == 0x02000004);
  CHECK(result.detail.find("undefined instruction") != std::string::npos);
  // The processor is left as it was before the instruction: in the mode the
  // program runs in, not the one the exception would have entered, with the
  // earlier instruction's result, and the undefined mode's registers as
  // they were.
  const ds::Cpu& cpu = console.machine().cpu();
  CHECK((cpu.cpsr_ctrl & 0x1Fu) == 0x1Fu);
  CHECK(cpu.r[0] == 7);
  CHECK(cpu.bank_r14[5] == 0 && cpu.bank_spsr[5] == 0);
}

namespace {

// A program for the interrupt tests. It installs a handler, enables the
// vertical blank interrupt and then does one of eight things, chosen by r7:
//
//   0  counts in a loop                       4  calls Halt in a loop
//   1  sleeps (system coprocessor) in a loop  5  waits for an interrupt that never comes
//   2  calls VBlankIntrWait in a loop         6  as 0, in Thumb state
//   3  as 2, in Thumb state                   7  as 1, with no interrupt enabled
//
// Each loop adds one to r8 per round (r8, because the BIOS calls leave it
// alone even while they are waiting). The handler acknowledges the
// interrupt, records it where IntrWait looks, and adds one to the word at
// 0x02001000.
//
//   02000000  mov r0, #0x04000000; ldr r1, =handler; ldr r2, =0x03003FFC; str r1, [r2]
//   02000010  mov r1, #8; strh r1, [r0, #4]            vertical blank interrupt on
//   02000018  add r2, r0, #0x200; mov r1, #1; cmp r7, #7; moveq r1, #0
//   02000028  str r1, [r2, #0x10]                      enable register
//   0200002c  mov r1, #1; str r1, [r2, #8]             master switch
//   02000034  mov r1, #0x1F; msr cpsr_c, r1            System mode, interrupts unmasked
//   0200003c  mov r8, #0; add pc, pc, r7, lsl #2; nop; eight branches
//   02000068  busy:        add r8, r8, #1; b busy
//   02000070  sleep:       mov r0, #0; mcr p15, 0, r0, c7, c0, 4; add r8, r8, #1; b sleep
//   02000080  vblank_wait: svc 0x050000; add r8, r8, #1; b vblank_wait
//   0200008c  halt_call:   svc 0x060000; add r8, r8, #1; b halt_call
//   02000098  wait_other:  mov r0, #1; mov r1, #2; svc 0x040000; add r8, r8, #1; b wait_other
//   020000ac  two jumps into Thumb state
//   020000bc  t_wait (Thumb): svc 5; movs r0, #1; add r8, r0; b t_wait
//   020000c4  t_busy (Thumb): movs r0, #1; add r8, r0; b t_busy
//   020000cc  handler: mov r0, #0x04000000; add r0, r0, #0x200; ldr r1, [r0, #0x14]; str r1, [r0, #0x14]
//             ldr r2, =0x03003FF8; ldr r3, [r2]; orr r3, r3, r1; str r3, [r2]
//             ldr r2, =0x02001000; ldr r3, [r2]; add r3, r3, #1; str r3, [r2]; bx lr
const std::vector<std::uint32_t> kInterrupts = {
    0xE3A00301, 0xE59F10F4, 0xE59F20F4, 0xE5821000, 0xE3A01008, 0xE1C010B4, 0xE2802C02,
    0xE3A01001, 0xE3570007, 0x03A01000, 0xE5821010, 0xE3A01001, 0xE5821008, 0xE3A0101F,
    0xE121F001, 0xE3A08000, 0xE08FF107, 0xE1A00000, 0xEA000006, 0xEA000007, 0xEA00000A,
    0xEA000014, 0xEA00000B, 0xEA00000D, 0xEA000013, 0xEA000001, 0xE2888001, 0xEAFFFFFD,
    0xE3A00000, 0xEE070F90, 0xE2888001, 0xEAFFFFFB, 0xEF050000, 0xE2888001, 0xEAFFFFFC,
    0xEF060000, 0xE2888001, 0xEAFFFFFC, 0xE3A00001, 0xE3A01002, 0xEF040000, 0xE2888001,
    0xEAFFFFFA, 0xE28F0009, 0xE12FFF10, 0xE28F0009, 0xE12FFF10, 0x2001DF05, 0xE7FB4480,
    0x44802001, 0x0000E7FC, 0xE3A00301, 0xE2800C02, 0xE5901014, 0xE5801014, 0xE59F2024,
    0xE5923000, 0xE1833001, 0xE5823000, 0xE59F2018, 0xE5923000, 0xE2833001, 0xE5823000,
    0xE12FFF1E, 0x020000CC, 0x03003FFC, 0x03003FF8, 0x02001000,
};

// When the fourth vertical blank begins: line 192 of the first frame, and
// three whole frames more.
constexpr std::uint64_t kFourthVerticalBlank = 192 * 2130 + 3 * (263 * 2130);

struct InterruptRun {
  bd::nds::Program program = program_of(kInterrupts);
  bd::nds::Console console{program, kNoTranslations};
  bd::nds::RunResult result;
  // Runs variant `variant` until the fourth vertical blank begins. Three
  // have been and gone by then; the run stops before the fourth is taken.
  explicit InterruptRun(std::uint32_t variant, std::uint64_t frames = 4) {
    if (!console.direct_boot().ok()) return;
    console.machine().cpu().r[7] = variant;
    console.machine().cpu().r[9] = 0xC0FFEE;  // Something for the handler to leave alone.
    result = console.run(50000000, frames);
  }
  std::uint32_t handled() { return console.bus().read32(0x02001000); }
  std::uint32_t rounds() { return console.machine().cpu().r[8]; }
};

}  // namespace

TEST(console_takes_interrupts_in_a_busy_program) {
  InterruptRun run(0);
  CHECK(run.result.reason == bd::nds::StopReason::FrameLimit);
  CHECK(run.handled() == 3);
  CHECK(run.console.machine().statistics().interrupts_taken == 3);
  CHECK(run.console.video_timing().frames() == 4);
  CHECK(run.console.sleeps() == 0);
  // The loop ran all the while. A round is two instructions and the run is
  // 2,089,530 cycles long at one instruction per cycle.
  CHECK(run.rounds() > 1040000 && run.rounds() < 1045000);
  // The interrupted program's registers came back as they were.
  CHECK(run.console.machine().cpu().r[9] == 0xC0FFEE);
  CHECK((ds::get_cpsr(run.console.machine().cpu()) & 0xFF) == 0x1F);
  CHECK(run.console.io().unmodelled().empty());
}

TEST(console_takes_interrupts_in_thumb_state) {
  InterruptRun run(6);
  CHECK(run.result.reason == bd::nds::StopReason::FrameLimit);
  CHECK(run.handled() == 3);
  CHECK(run.rounds() > 693000 && run.rounds() < 697000);  // Three instructions a round here.
}

TEST(console_sleeps_until_an_interrupt) {
  InterruptRun run(1);
  CHECK(run.result.reason == bd::nds::StopReason::FrameLimit);
  CHECK(run.handled() == 3);
  CHECK(run.rounds() == 3);  // One round per interrupt: asleep in between.
  CHECK(run.console.sleeps() == 4);
  // Time jumped from one vertical blank to the next: the run ends exactly
  // when the fourth begins.
  CHECK(run.console.timeline().now() == kFourthVerticalBlank);
  CHECK(run.console.machine().statistics().interpreted_instructions < 200);
}

TEST(console_bios_calls_that_wait) {
  for (std::uint32_t variant : {2u, 3u, 4u}) {  // VBlankIntrWait in ARM and Thumb, Halt.
    InterruptRun run(variant);
    CHECK(run.result.reason == bd::nds::StopReason::FrameLimit);
    CHECK(run.handled() == 3);
    CHECK(run.rounds() == 3);
    CHECK(run.console.timeline().now() == kFourthVerticalBlank);
    CHECK(run.console.software_interrupts().at(variant == 4 ? 6 : 5) == 4);
    CHECK(run.console.machine().cpu().r[9] == 0xC0FFEE);
  }
}

TEST(console_interrupt_wait_ignores_other_interrupts) {
  // IntrWait for an interrupt that never comes: the vertical blank interrupts
  // are handled meanwhile, and the call does not return.
  InterruptRun run(5);
  CHECK(run.result.reason == bd::nds::StopReason::FrameLimit);
  CHECK(run.handled() == 3);
  CHECK(run.rounds() == 0);
  CHECK(run.console.software_interrupts().at(4) == 1);
}

TEST(console_gives_up_on_a_sleep_that_never_ends) {
  // No interrupt is enabled, so nothing can wake the processor.
  InterruptRun run(7, 0);
  CHECK(run.result.reason == bd::nds::StopReason::Unhandled);
  CHECK(run.result.detail.find("ten seconds") != std::string::npos);
  CHECK(run.handled() == 0 && run.rounds() == 0);
  // It gave up at the first event more than ten seconds on, no later.
  const std::uint64_t ten_seconds = 10 * bd::nds::Timeline::kCyclesPerSecond;
  CHECK(run.console.timeline().now() <= ten_seconds);
  CHECK(run.console.timeline().now() > ten_seconds - 2 * 2130);
}

TEST(console_stops_when_execution_leaves_memory) {
  //   02000000  mov r0, #0x08000000
  //   02000004  bx  r0               nothing is mapped there
  const bd::nds::Program program = program_of({0xE3A00302, 0xE12FFF10});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::Unhandled);
  CHECK(result.address == 0x08000000);
  CHECK(result.detail.find("no memory") != std::string::npos);
  CHECK(console.machine().statistics().interpreted_instructions == 2);
}

TEST(console_program_reads_the_cartridge) {
  // Reads one word from the cartridge the way a program does: the command,
  // the transfer control, a wait for the word, the data register.
  //   02000000  mov r0, #0x04000000; add r1, r0, #0x1A0
  //   02000008  ldr r2, =0x000100B7; str r2, [r1, #8]     command: B7 00 01 00 ...
  //   02000010  mov r2, #0x10; str r2, [r1, #0xC]         ... 10 00 00 00: read data at 0x00010010
  //   02000018  mov r2, #0x87000000; str r2, [r1, #4]     start, four bytes
  //   02000020  ldr r3, [r1, #4]; tst r3, #0x00800000; beq 0x02000020
  //   0200002c  ldr r3, =0x04100010; ldr r4, [r3]
  //   02000034  ldr r5, [r1, #4]
  //   02000038  b .
  const bd::nds::Program program =
      program_of({0xE3A00301, 0xE2801E1A, 0xE59F202C, 0xE5812008, 0xE3A02010, 0xE581200C, 0xE3A02487, 0xE5812004, 0xE5913004,
                  0xE3130502, 0x0AFFFFFC, 0xE59F300C, 0xE5934000, 0xE5915004, 0xEAFFFFFE, 0x000100B7, 0x04100010});
  std::vector<std::uint8_t> rom(0x20000);
  for (std::uint32_t a = 0; a < rom.size(); a += 4) {
    for (int b = 0; b < 4; ++b) rom[a + b] = static_cast<std::uint8_t>((a ^ 0xA5000000u) >> (8 * b));
  }
  bd::nds::Console console(program, kNoTranslations);
  console.insert_cartridge(bd::ByteSpan(rom.data(), rom.size()));
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::InstructionLimit);
  CHECK(result.address == 0x02000038);
  CHECK(console.machine().cpu().r[4] == (0x00010010u ^ 0xA5000000u));
  CHECK((console.machine().cpu().r[5] & 0x80800000u) == 0);  // Afterwards: not busy, no word waiting.
  CHECK(console.cartridge().statistics().data_reads == 1 && console.cartridge().statistics().data_bytes == 4);
  CHECK(console.io().unmodelled().empty());

  // The same program with the slot empty reads all ones.
  bd::nds::Console empty(program, kNoTranslations);
  CHECK(empty.direct_boot().ok());
  (void)empty.run(1000);
  CHECK(empty.machine().cpu().r[4] == 0xFFFFFFFFu);
  CHECK(empty.cartridge().statistics().reads_without_cartridge == 1);
}

TEST(console_shows_its_screens_a_line_at_a_time) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  // Both engines show their picture, which is only a backdrop: red for the
  // main engine, green for the second.
  console.io().write(0x04000000, 4, 1u << 16);
  console.io().write(0x04001000, 4, 1u << 16);
  console.bus().write16(0x05000000, 0x001F);
  console.bus().write16(0x05000400, 0x03E0);
  // A hundred lines in, the main engine's backdrop is made blue: lines
  // already drawn stay as they were. Then on to the vertical blank.
  CHECK(console.run(100 * 2130).reason == bd::nds::StopReason::InstructionLimit);
  CHECK(console.video_timing().line() >= 99 && console.video_timing().line() < 101);
  const unsigned changed_at = console.video_timing().line() + 1;
  console.bus().write16(0x05000000, 0x7C00);
  console.run(1000000, 1);
  CHECK(console.display().statistics().frames == 1);
  // The main engine is on the bottom screen.
  const std::vector<std::uint32_t>& top = console.display().top_screen();
  const std::vector<std::uint32_t>& bottom = console.display().bottom_screen();
  const std::uint32_t red = 62, green = 62u << 6, blue = 62u << 12;
  CHECK(top[0] == green && top[191 * 256 + 255] == green);
  // The line under way when the change came was already drawn if its dots
  // were done; either way the next line is blue.
  CHECK(bottom[0] == red && bottom[98 * 256 + 255] == red);
  CHECK(bottom[changed_at * 256] == blue && bottom[191 * 256 + 255] == blue);
}

TEST(console_calls_on_frame_at_each_vertical_blank_and_ends_when_asked) {
  //   02000000  b    .
  const bd::nds::Program program = program_of({0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  console.io().write(0x04000000, 4, 1u << 16);
  console.bus().write16(0x05000000, 0x001F);
  int calls = 0;
  bool screens_complete = true;
  console.on_frame = [&] {
    ++calls;
    // The frame is shown before the callback, its last line drawn.
    screens_complete = screens_complete && console.display().statistics().frames == static_cast<std::uint64_t>(calls) &&
                       console.display().bottom_screen()[191 * 256 + 255] == 62u;
    console.keypad().set_held(1u << static_cast<unsigned>(bd::nds::Button::Start));
    return calls < 3;
  };
  // Asked to end on the third frame, though the frame limit is reached there too.
  const bd::nds::RunResult result = console.run(10000000, 3);
  CHECK(result.reason == bd::nds::StopReason::Asked);
  CHECK(calls == 3);
  CHECK(screens_complete);
  // What the callback held, the program reads: Start is bit 3, 0 while held.
  CHECK(console.io().read(0x04000130, 2) == (0x03FFu & ~(1u << 3)));
  // A run that is not asked to end stops at its own limit.
  console.on_frame = [&] { return true; };
  CHECK(console.run(10000000, 5).reason == bd::nds::StopReason::FrameLimit);
}
