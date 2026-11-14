// The DS as the game's main program sees it: processor, memory, system
// coprocessor and hardware registers, put together and started the way the
// console's firmware starts a game.
//
// This is a first assembly. Some BIOS calls are implemented (bios.h); there
// is no timing, interrupts are requested but never taken, and the second
// processor's stand-in only answers the start-up handshake. Whatever the
// program asks for that is not there is recorded, and a BIOS call that is
// not implemented stops the run.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/bios.h"
#include "nds/dma.h"
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
  Dma& dma() { return dma_; }
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
  // Memory as the BIOS and DMA write it: like the program's own writes, theirs
  // must be seen by the machine, which watches for code being replaced.
  class WatchedBus : public runtime::Bus {
   public:
    WatchedBus(Arm9Bus& bus, runtime::Machine& machine) : bus_(bus), machine_(machine) {}
    std::uint8_t read8(std::uint32_t a) override { return bus_.read8(a); }
    std::uint16_t read16(std::uint32_t a) override { return bus_.read16(a); }
    std::uint32_t read32(std::uint32_t a) override { return bus_.read32(a); }
    void write8(std::uint32_t a, std::uint8_t v) override {
      machine_.watch_write(a);
      bus_.write8(a, v);
    }
    void write16(std::uint32_t a, std::uint16_t v) override {
      machine_.watch_write(a);
      bus_.write16(a, v);
    }
    void write32(std::uint32_t a, std::uint32_t v) override {
      machine_.watch_write(a);
      bus_.write32(a, v);
    }

   private:
    Arm9Bus& bus_;
    runtime::Machine& machine_;
  };
  WatchedBus watched_bus_;
  Bios bios_;
  Dma dma_;
  std::map<std::uint32_t, std::uint64_t> software_interrupts_;
  bool stopped_ = false;
  std::uint32_t stop_address_ = 0;
  std::string stop_detail_;
};

}  // namespace bd::nds
