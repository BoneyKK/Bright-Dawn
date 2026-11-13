// The console runs small hand-assembled programs. The words were produced
// with the same assembler as the guest tests; the source is beside each.
#include <cstring>
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
  // Both hardware register accesses were recorded.
  const auto& registers = console.io().unmodelled();
  CHECK(registers.at(0x04000208).writes == 1 && registers.at(0x04000208).last_written == 0x04000000);
  CHECK(registers.at(0x04000208).size == 4);
  CHECK(registers.at(0x04000006).reads == 1 && registers.at(0x04000006).size == 2);
  CHECK(console.software_interrupts().empty());
  CHECK(console.machine().statistics().interpreted_instructions == 1000);
}

TEST(console_stops_at_a_software_interrupt) {
  //   02000000  mov r0, #1
  //   02000004  svc 0x030000
  //   02000008  mov r0, #2
  //   0200000c  b   .
  const bd::nds::Program program = program_of({0xE3A00001, 0xEF030000, 0xE3A00002, 0xEAFFFFFE});
  bd::nds::Console console(program, kNoTranslations);
  CHECK(console.direct_boot().ok());
  const bd::nds::RunResult result = console.run(1000);
  CHECK(result.reason == bd::nds::StopReason::Unhandled);
  CHECK(result.address == 0x02000004);
  CHECK(result.detail.find("software interrupt 3") != std::string::npos);
  CHECK(console.software_interrupts().at(3) == 1);
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
}
