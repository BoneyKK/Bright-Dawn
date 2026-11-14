#include "nds/console.h"

#include "nds/bios_rom.h"
#include "rom/arm9.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

// Where the firmware leaves a copy of the start of the ROM header.
constexpr u32 kHeaderCopy = 0x027FFE00;

}  // namespace

Console::Console(const Program& program, const TranslatedEntry* translated)
    : program_(program),
      bus_(io_),
      system_control_(bus_),
      video_timing_(timeline_, interrupts_),
      timers_(timeline_, interrupts_),
      second_processor_(bus_),
      ipc_(interrupts_, second_processor_),
      machine_(bus_, *this, translated),
      watched_bus_(bus_, machine_),
      bios_(watched_bus_),
      dma_(watched_bus_, interrupts_),
      cartridge_(interrupts_) {
  interrupts_.attach(io_);
  ipc_.attach(io_);
  dma_.attach(io_);
  video_timing_.attach(io_);
  timers_.attach(io_);
  cartridge_.attach(io_);
  math_unit_.attach(io_);
  // A DMA channel can be set to take each word the cartridge delivers.
  cartridge_.on_word_ready = [this] { return dma_.trigger(Dma::kStartCartridge); };
  dma_.on_waiting = [this](unsigned mode) {
    if (mode == Dma::kStartCartridge) cartridge_.pump();
  };
  interrupts_.on_change = [this] { machine_.set_interrupt_line(interrupts_.pending()); };
  timeline_.on_refresh = [this] { catch_up(); };
  timeline_.on_rescheduled = [this] { set_alarm(); };
  video_timing_.on_vertical_blank = [this] {
    if (frame_limit_ != 0 && video_timing_.frames() >= frame_limit_ && !frame_limit_reached_) {
      frame_limit_reached_ = true;
      machine_.stop();
    }
  };
  second_processor_.connect(ipc_);
}

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
  // This project's own code where the BIOS would be (bios_rom.S).
  std::vector<std::uint8_t> bios(kBiosRomWords * 4);
  for (std::size_t i = 0; i < kBiosRomWords; ++i) {
    for (int b = 0; b < 4; ++b) bios[i * 4 + b] = static_cast<std::uint8_t>(kBiosRom[i] >> (8 * b));
  }
  bus_.set_bios(bios.data(), bios.size());

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

RunResult Console::run(std::uint64_t instruction_limit, std::uint64_t frame_limit) {
  stopped_ = false;
  stop_detail_.clear();
  frame_limit_ = frame_limit;
  frame_limit_reached_ = false;
  machine_.set_instruction_limit(instruction_limit);
  accounted_instructions_ = machine_.statistics().interpreted_instructions;
  set_alarm();
  machine_.call_function(program_.arm9_entry, kResetVector);
  RunResult result;
  if (stopped_) {
    result.reason = StopReason::Unhandled;
    result.address = stop_address_;
    result.detail = stop_detail_;
  } else if (frame_limit_reached_) {
    result.reason = StopReason::FrameLimit;
  } else if (machine_.instruction_limit_reached() && instruction_limit != 0) {
    result.reason = StopReason::InstructionLimit;
    result.address = limit_address_;
  } else {
    result.reason = StopReason::Returned;
  }
  return result;
}

void Console::catch_up() {
  const std::uint64_t instructions = machine_.statistics().interpreted_instructions;
  const std::uint64_t passed = instructions - accounted_instructions_;
  accounted_instructions_ = instructions;
  if (passed != 0) timeline_.advance(passed * kCyclesPerInstruction);
  set_alarm();
}

void Console::set_alarm() {
  const Timeline::Time next = timeline_.next_event();
  if (next == Timeline::kNever) return;
  // Time not yet accounted for counts towards the event too.
  const std::uint64_t pending = machine_.statistics().interpreted_instructions - accounted_instructions_;
  const Timeline::Time now = timeline_.now() + pending * kCyclesPerInstruction;
  const Timeline::Time wait = next > now ? next - now : 0;
  machine_.set_alarm(wait / kCyclesPerInstruction + 1);
}

void Console::alarm() { catch_up(); }

void Console::wait_for_interrupt() {
  ++sleeps_;
  catch_up();
  const Timeline::Time began = timeline_.now();
  // What ends the sleep is a request from an enabled source; the master
  // switch and the processor's mask decide only whether it is then taken.
  while (!interrupts_.any_enabled_requested() && !machine_.stopped()) {
    const Timeline::Time next = timeline_.next_event();
    if (next == Timeline::kNever) {
      stop(0, "the processor is waiting for an interrupt and nothing is going to raise one");
      machine_.stop();
      return;
    }
    if (next - began > kLongestSleep) {
      stop(0, "the processor waited ten seconds of console time for an interrupt that did not come");
      machine_.stop();
      return;
    }
    timeline_.advance_to(next);
  }
  set_alarm();
}

u32 Console::exception_vectors() { return system_control_.high_vectors() ? 0xFFFF0000u : 0u; }

void Console::stop(u32 address, const std::string& what) {
  if (stopped_) return;  // Keep the first cause.
  stopped_ = true;
  stop_address_ = address;
  stop_detail_ = what;
}

u32 Console::software_interrupt(ds::Cpu& cpu, u32 number, u32 return_address) {
  ++software_interrupts_[number];
  // The three calls that wait for an interrupt run as code on the processor
  // (bios_rom.S), because the program's interrupt handler runs in the middle
  // of them. The processor takes the software interrupt exception for real.
  if (number == 0x04 || number == 0x05 || number == 0x06) {
    machine_.enter_exception(0x13, return_address & ~1u, (return_address & 1u) != 0);
    return exception_vectors() + 0x08;
  }
  const Bios::Result result = bios_.call(cpu, number);
  if (result.done) return return_address;
  // Carrying on with registers the call should have changed would only
  // produce nonsense further down, so the run ends here.
  const u32 size = (return_address & 1u) ? 2 : 4;
  stop((return_address & ~1u) - size, result.problem);
  machine_.stop();
  return return_address;
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
  if (system_control_.wait_requested()) {
    system_control_.clear_wait_request();
    wait_for_interrupt();
  }
}

void Console::unhandled(ds::Cpu&, u32 address, const char* what) {
  if (machine_.instruction_limit_reached()) {
    limit_address_ = address;
    return;
  }
  stop(address, what);
}

}  // namespace bd::nds
