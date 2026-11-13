#include "nds/console.h"

#include "rom/arm9.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

// Where the firmware leaves a copy of the start of the ROM header.
constexpr u32 kHeaderCopy = 0x027FFE00;

}  // namespace

Console::Console(const Program& program, const TranslatedEntry* translated)
    : program_(program), bus_(io_), system_control_(bus_), machine_(bus_, *this, translated) {}

Status Console::direct_boot() {
  // What follows is the state a game finds when the firmware starts it, as
  // far as it is known to matter. The values are the ones emulators set when
  // they skip the firmware (checked against NooDS) and GBATEK documents.
  system_control_.set_firmware_state();
  io_.set(0x04000247, 1, 0x03);    // Shared memory: all of it to the second processor.
  io_.set(0x04000300, 1, 0x01);    // "The firmware has run."
  io_.set(0x04000304, 2, 0x0001);  // Power: screens on.

  // The main program. The image is already decompressed, so the field that
  // tells the start-up code where the compressed data ends has to be zero, or
  // that code would run its decompressor over the program a second time.
  std::vector<std::uint8_t> arm9 = program_.arm9;
  if (const auto params = rom::find_module_params(arm9)) {
    const std::size_t field = params->offset + 0x14;
    if (field + 4 <= arm9.size()) arm9[field] = arm9[field + 1] = arm9[field + 2] = arm9[field + 3] = 0;
  }
  if (!bus_.load(program_.arm9_load_address, arm9.data(), arm9.size()))
    return Error{ErrorCode::Corrupt, "the ARM9 program does not fit in main memory at its load address"};
  machine_.note_memory_written(program_.arm9_load_address, arm9.size());

  // Values at the top of main memory.
  bus_.write32(0x027FF800, 0x00001FC2);  // Cartridge chip ID, twice.
  bus_.write32(0x027FF804, 0x00001FC2);
  bus_.write16(0x027FF850, 0x5835);      // Checksum of the second processor's BIOS.
  bus_.write16(0x027FF880, 0x0007);      // Start-up message between the processors.
  bus_.write16(0x027FF884, 0x0006);
  bus_.write32(0x027FFC00, 0x00001FC2);  // Copies of the above.
  bus_.write32(0x027FFC04, 0x00001FC2);
  bus_.write16(0x027FFC10, 0x5835);
  bus_.write16(0x027FFC40, 0x0001);      // Started from a cartridge.
  for (std::size_t i = 0; i < program_.header.size() && i < 0x170; ++i)
    bus_.write8(kHeaderCopy + static_cast<u32>(i), program_.header[i]);

  for (const OverlayCode& overlay : program_.overlays)
    machine_.add_overlay(overlay.id, overlay.load_address, overlay.image.data(), overlay.image.size());

  // Processor: System mode with interrupts masked, and a stack for each of
  // the three modes a program uses.
  machine_.reset_cpu();
  ds::Cpu& c = machine_.cpu();
  ds::switch_mode(c, 0x12);
  c.r[13] = 0x03003F80;
  ds::switch_mode(c, 0x13);
  c.r[13] = 0x03003FC0;
  ds::switch_mode(c, 0x1F);
  c.r[13] = 0x03002F7C;
  c.r[12] = program_.arm9_entry;
  c.cpsr_ctrl = 0xDF;
  return {};
}

RunResult Console::run(std::uint64_t instruction_limit) {
  stopped_ = false;
  stop_detail_.clear();
  machine_.set_instruction_limit(instruction_limit);
  machine_.call_function(program_.arm9_entry, kResetVector);
  RunResult result;
  if (machine_.instruction_limit_reached() && instruction_limit != 0) {
    result.reason = StopReason::InstructionLimit;
    result.address = stop_address_;
  } else if (stopped_) {
    result.reason = StopReason::Unhandled;
    result.address = stop_address_;
    result.detail = stop_detail_;
  } else {
    result.reason = StopReason::Returned;
  }
  return result;
}

void Console::stop(u32 address, const std::string& what) {
  if (stopped_) return;  // Keep the first cause.
  stopped_ = true;
  stop_address_ = address;
  stop_detail_ = what;
}

void Console::software_interrupt(ds::Cpu&, u32 number, u32 return_address) {
  ++software_interrupts_[number];
  // There is no BIOS yet. Carrying on with registers the call should have
  // changed would only produce nonsense further down, so the run ends here.
  const u32 size = (return_address & 1u) ? 2 : 4;
  stop((return_address & ~1u) - size, "software interrupt " + std::to_string(number) + " (the BIOS is not implemented)");
  machine_.stop();
}

u32 Console::coprocessor_read(ds::Cpu&, int cp, int opc1, int crn, int crm, int opc2) {
  if (cp != 15 || opc1 != 0) {
    stop(0, "read of a coprocessor register that does not exist");
    machine_.stop();
    return 0;
  }
  return system_control_.read(crn, crm, opc2);
}

void Console::coprocessor_write(ds::Cpu&, int cp, int opc1, int crn, int crm, int opc2, u32 value) {
  if (cp != 15 || opc1 != 0) {
    stop(0, "write to a coprocessor register that does not exist");
    machine_.stop();
    return;
  }
  system_control_.write(crn, crm, opc2, value);
}

void Console::unhandled(ds::Cpu&, u32 address, const char* what) { stop(address, what); }

}  // namespace bd::nds
