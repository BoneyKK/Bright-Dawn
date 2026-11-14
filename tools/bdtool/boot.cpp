// `bdtool boot`: runs the game's start-up code and reports what it asked of
// the machine. A diagnostic for developing the runtime, not a way to play.
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/version.h"
#include "nds/console.h"
#include "nds/program.h"
#include "rom/rom.h"
#include "translated_table.h"

namespace {

// This tool has no translated code linked in: everything is interpreted.
const TranslatedEntry kNoTranslations[] = {{0, nullptr, -1, nullptr, 0}};

const char* mode_name(std::uint32_t mode) {
  switch (mode & 0x1Fu) {
    case 0x10: return "User";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "Supervisor";
    case 0x17: return "Abort";
    case 0x1B: return "Undefined";
    case 0x1F: return "System";
    default: return "invalid";
  }
}

}  // namespace

int cmd_boot(const std::filesystem::path& source, std::uint64_t instruction_limit) {
  using namespace bd;
  std::printf("bdtool %s\n", BD_VERSION);

  std::error_code ec;
  const bool is_directory = std::filesystem::is_directory(source, ec);
  Result<nds::Program> loaded = Error{ErrorCode::Io, "not loaded"};
  if (is_directory) {
    loaded = nds::program_from_directory(source);
  } else {
    auto rom = rom::Rom::open(source);
    if (!rom) {
      std::fprintf(stderr, "error: %s\n", rom.error().message.c_str());
      return 1;
    }
    loaded = nds::program_from_rom(rom.value());
  }
  if (!loaded) {
    std::fprintf(stderr, "error: %s\n", loaded.error().message.c_str());
    return 1;
  }
  const nds::Program& program = loaded.value();

  nds::Console console(program, kNoTranslations);
  if (auto s = console.direct_boot(); !s) {
    std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
    return 1;
  }
  const nds::RunResult result = console.run(instruction_limit);

  std::printf("Boot trace. Every instruction is interpreted; this tool contains no translated code.\n");
  std::printf("Program: ARM9 %zu bytes at 0x%08" PRIX32 ", entry 0x%08" PRIX32 ", %zu overlays, header %s\n",
              program.arm9.size(), program.arm9_load_address, program.arm9_entry, program.overlays.size(),
              program.header.empty() ? "not available" : "available");

  const auto& stats = console.machine().statistics();
  std::printf("Instructions interpreted: %" PRIu64 "\n", stats.interpreted_instructions);
  switch (result.reason) {
    case nds::StopReason::Returned:
      std::printf("Stopped: the program returned to the reset vector\n");
      break;
    case nds::StopReason::InstructionLimit:
      std::printf("Stopped: instruction limit reached, at 0x%08" PRIX32 "\n", result.address);
      break;
    case nds::StopReason::Unhandled:
      std::printf("Stopped: at 0x%08" PRIX32 ": %s\n", result.address, result.detail.c_str());
      break;
  }

  const ds::Cpu& c = console.machine().cpu();
  const std::uint32_t status = ds::get_cpsr(const_cast<ds::Cpu&>(c));
  std::printf("\nProcessor: %s mode, status 0x%08" PRIX32 "\n", mode_name(status), status);
  for (int i = 0; i < 15; ++i)
    std::printf("  r%-2d 0x%08" PRIX32 "%s", i, c.r[i], (i % 4 == 3 || i == 14) ? "\n" : "");

  nds::SystemControl& cp = console.system_control();
  std::printf("\nSystem coprocessor: control 0x%08" PRIX32 ", data memory register 0x%08" PRIX32
              ", instruction memory register 0x%08" PRIX32 "\n",
              cp.control(), cp.read(9, 1, 0), cp.read(9, 1, 1));
  for (const auto& [key, count] : cp.unknown_reads())
    std::printf("  unknown register c%u,c%u,%u read %" PRIu64 " times\n", key >> 8, (key >> 4) & 15, key & 15, count);
  for (const auto& [key, count] : cp.unknown_writes())
    std::printf("  unknown register c%u,c%u,%u written %" PRIu64 " times\n", key >> 8, (key >> 4) & 15, key & 15, count);

  std::printf("\nBIOS calls: %s\n", console.software_interrupts().empty() ? "none" : "");
  for (const auto& [number, count] : console.software_interrupts()) {
    const char* name = nds::Bios::name(number);
    std::printf("  0x%02" PRIX32 " %-16s %" PRIu64 "\n", number, name ? name : "(unknown)", count);
  }

  nds::Interrupts& irq = console.interrupts();
  std::printf("\nInterrupts: master switch %s, enabled 0x%08" PRIX32 ", requested 0x%08" PRIX32
              " (none is ever taken yet)\n",
              irq.master() ? "on" : "off", irq.enabled(), irq.requested());

  nds::SecondProcessor& second = console.second_processor();
  std::printf("\nSecond processor: start-up handshake %s\n", second.handshake_done() ? "completed" : "not completed");
  if (!second.words_by_service().empty()) std::printf("  words sent to it, which nothing answers yet:\n");
  for (const auto& [service, count] : second.words_by_service())
    std::printf("    service %2u: %" PRIu64 "\n", service, count);
  if (!second.first_words().empty()) {
    std::printf("  the first %zu, in order:", second.first_words().size());
    for (std::size_t i = 0; i < second.first_words().size(); ++i)
      std::printf("%s0x%08" PRIX32, i % 6 == 0 ? "\n    " : " ", second.first_words()[i]);
    std::printf("\n");
  }

  const nds::Dma::Statistics& dma = console.dma().statistics();
  std::printf("\nDMA: %" PRIu64 " transfers performed, %" PRIu64 " units moved\n", dma.transfers, dma.units);
  for (int mode = 1; mode < 8; ++mode) {
    if (dma.waiting_for_event[mode] != 0)
      std::printf("  %" PRIu64 " started in mode %d, which waits for an event that never comes yet\n",
                  dma.waiting_for_event[mode], mode);
  }

  const auto& registers = console.io().unmodelled();
  std::printf("\nHardware registers used that have no model: %zu\n", registers.size());
  if (!registers.empty()) std::printf("  address     width  reads      writes     last value written\n");
  for (const auto& [address, use] : registers) {
    std::printf("  0x%08" PRIX32 "  %u      %-10" PRIu64 " %-10" PRIu64 " ", address, use.size, use.reads, use.writes);
    if (use.writes != 0) std::printf("0x%0*" PRIX32, static_cast<int>(use.size * 2), use.last_written);
    std::printf("\n");
  }

  const auto& bus = console.bus().statistics();
  std::printf("\nMemory accessed where nothing is mapped: %s\n",
              bus.unmapped_reads.empty() && bus.unmapped_writes.empty() ? "none" : "");
  for (const auto& [block, count] : bus.unmapped_reads)
    std::printf("  0x%04" PRIX32 "0000: %" PRIu64 " reads\n", block, count);
  for (const auto& [block, count] : bus.unmapped_writes)
    std::printf("  0x%04" PRIX32 "0000: %" PRIu64 " writes\n", block, count);
  return 0;
}
