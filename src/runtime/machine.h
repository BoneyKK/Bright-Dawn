// Runs translated guest code, with an interpreter behind it for everything
// that has no translation.
//
// The rule is that translated code is always preferred. The interpreter is
// entered only at an address with no translation, and it hands back at the
// first opportunity: every call or jump it makes to a translated function
// runs that function natively.
//
// How the two fit together. Translated functions keep all of their state in
// the processor model and in guest memory, never in native variables that
// outlive an instruction. The native call stack is therefore only a way of
// running fast: a native frame can be abandoned at any time and the program
// continued from the model alone. Every translated call site registers the
// return address it is waiting for (ds::enter / ds::leave). When control
// arrives at such an address with the stack pointer the call site had, by
// any route (a plain return, a return through a register, a jump out of
// several functions at once), execution resumes natively at that call site,
// and any call sites inside it are told to give up.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "ds_cpu.h"
#include "translated_table.h"

struct bd_arm_core;
struct bd_arm_state;

namespace bd::runtime {

// Guest memory. Addresses given to the 16- and 32-bit functions are aligned.
class Bus {
 public:
  virtual ~Bus() = default;
  virtual std::uint8_t read8(std::uint32_t address) = 0;
  virtual std::uint16_t read16(std::uint32_t address) = 0;
  virtual std::uint32_t read32(std::uint32_t address) = 0;
  virtual void write8(std::uint32_t address, std::uint8_t value) = 0;
  virtual void write16(std::uint32_t address, std::uint16_t value) = 0;
  virtual void write32(std::uint32_t address, std::uint32_t value) = 0;
};

// What lies outside the processor. Handlers receive the processor model;
// during interpretation its registers are current only for software
// interrupts, not for coprocessor access.
class System {
 public:
  virtual ~System() = default;
  virtual void software_interrupt(ds::Cpu& cpu, std::uint32_t number) = 0;
  virtual std::uint32_t coprocessor_read(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2) = 0;
  virtual void coprocessor_write(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2,
                                 std::uint32_t value) = 0;
  // Something that cannot be executed correctly. The processor state is not
  // meaningful afterwards; the embedder should stop.
  virtual void unhandled(ds::Cpu& cpu, std::uint32_t address, const char* what) = 0;
};

struct Statistics {
  // How often the interpreter was entered, by the address it started at (bit
  // 0 set = Thumb). Each address is a gap in the translation.
  std::map<std::uint32_t, std::uint64_t> interpreter_entries;
  std::uint64_t interpreted_instructions = 0;
  // Hand-backs from interpreted code to translated functions.
  std::uint64_t native_calls_from_interpreter = 0;
  std::uint64_t native_jumps_from_interpreter = 0;
  // Computed jumps that turned out to be returns to a waiting call site.
  std::uint64_t returns_through_jumps = 0;
  // Calls whose callee returned somewhere other than the call site.
  std::uint64_t returns_elsewhere = 0;
  // Call sites told to give up because control went to an outer one.
  std::uint64_t call_sites_abandoned = 0;
};

class Machine {
 public:
  // `table` ends with an entry whose function is null. All three arguments
  // must outlive the machine.
  Machine(Bus& bus, System& system, const TranslatedEntry* table);
  ~Machine();
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  ds::Cpu& cpu() { return cpu_; }
  // Returns the processor to its initial state.
  void reset_cpu();

  // Calls the guest function at `address` (bit 0 set = Thumb) and returns when
  // control comes back to `return_address` with the stack pointer unchanged.
  // Arguments and results are exchanged through cpu().
  void call_function(std::uint32_t address, std::uint32_t return_address);

  // Whether a call or jump to `address` would run translated code.
  bool is_translated(std::uint32_t address) const { return lookup(address) != nullptr; }

  const Statistics& statistics() const { return statistics_; }
  void clear_statistics() { statistics_ = Statistics(); }

  // Stops interpretation, as unhandled, after this many instructions within
  // one call_function. 0 means no limit. For tests.
  void set_instruction_limit(std::uint64_t limit) { instruction_limit_ = limit; }

  // Entry points for the functions the translated code calls (ds::call,
  // ds::jump, ds::leave_slow). Not for other use.
  void dispatch_call(std::uint32_t address);
  void dispatch_jump(std::uint32_t address);
  bool leave_slow(std::uint32_t return_address);
  Bus& bus() { return bus_; }
  System& system() { return system_; }

 private:
  using Function = void (*)(ds::Cpu&);
  Function lookup(std::uint32_t address) const;
  int waiting_call_site(std::uint32_t address, std::uint32_t stack_pointer) const;
  void arrive(int call_site);
  void run_from(std::uint32_t address);
  bool interpret(std::uint32_t start, std::uint32_t* continue_at);
  void load_interpreter(std::uint32_t address, bd_arm_state* state);
  void store_from_interpreter(const bd_arm_state& state);
  void give_up(std::uint32_t address, const char* what);

  Bus& bus_;
  System& system_;
  std::vector<TranslatedEntry> table_;  // Sorted by address.
  bd_arm_core* core_ = nullptr;
  ds::Cpu cpu_;
  Statistics statistics_;
  std::uint64_t instruction_limit_ = 0;
  std::uint64_t instructions_this_call_ = 0;
};

}  // namespace bd::runtime
