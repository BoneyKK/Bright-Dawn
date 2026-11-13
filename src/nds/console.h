// The DS as the game's main program sees it: processor, memory, system
// coprocessor and hardware registers, put together and started the way the
// console's firmware starts a game.
//
// This is a first assembly. There is no BIOS behind software interrupts and
// no timing, interrupts are requested but never taken, and the second
// processor's stand-in only answers the start-up handshake. Whatever the
// program asks for that is not there is recorded, and a software interrupt
// stops the run.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/ipc.h"
#include "nds/program.h"
#include "nds/second_processor.h"
#include "nds/system_control.h"
#include "machine.h"

namespace bd::nds {

enum class StopReason {
  Returned,          // Control came back to where the entry point was called from.
  InstructionLimit,  // The requested number of interpreted instructions ran.
  Unhandled,         // The program needs something that is not implemented.
};

struct RunResult {
  StopReason reason = StopReason::Unhandled;
  std::uint32_t address = 0;  // Where it stopped, for the last two.
  std::string detail;
};

class Console : private runtime::System {
 public:
  // The address the entry point "returns" to. A DS program never returns; if
  // it jumps here, to the reset vector, it has ended.
  static constexpr std::uint32_t kResetVector = 0xFFFF0000u;

  // `program` and `translated` (a table ending with a null function; may be
  // empty) must outlive the console.
  Console(const Program& program, const TranslatedEntry* translated);

  // Places the program in memory and sets registers and memory the way the
  // firmware leaves them. Returns an error if the program does not fit.
  Status direct_boot();

  // Runs from the entry point. `instruction_limit` bounds the instructions
  // interpreted (0 = no bound); translated code is not counted.
  RunResult run(std::uint64_t instruction_limit);

  Arm9Bus& bus() { return bus_; }
  Arm9Io& io() { return io_; }
  SystemControl& system_control() { return system_control_; }
  Interrupts& interrupts() { return interrupts_; }
  Ipc& ipc() { return ipc_; }
  SecondProcessor& second_processor() { return second_processor_; }
  runtime::Machine& machine() { return machine_; }

  // Software interrupts the program made, by number.
  const std::map<std::uint32_t, std::uint64_t>& software_interrupts() const { return software_interrupts_; }

 private:
  void software_interrupt(ds::Cpu& cpu, std::uint32_t number, std::uint32_t return_address) override;
  std::uint32_t coprocessor_read(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2) override;
  void coprocessor_write(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2,
                         std::uint32_t value) override;
  void unhandled(ds::Cpu& cpu, std::uint32_t address, const char* what) override;
  void stop(std::uint32_t address, const std::string& what);

  const Program& program_;
  Arm9Io io_;
  Arm9Bus bus_;
  SystemControl system_control_;
  Interrupts interrupts_;
  SecondProcessor second_processor_;
  Ipc ipc_;
  runtime::Machine machine_;
  std::map<std::uint32_t, std::uint64_t> software_interrupts_;
  bool stopped_ = false;
  std::uint32_t stop_address_ = 0;
  std::string stop_detail_;
};

}  // namespace bd::nds
