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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
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
  // Whether there is memory at `address` that code could be in. The
  // interpreter stops when control goes where there is none.
  virtual bool has_code_memory(std::uint32_t address) {
    (void)address;
    return true;
  }
};

// What lies outside the processor. Handlers receive the processor model;
// during interpretation its registers are current only for software
// interrupts, not for coprocessor access.
class System {
 public:
  virtual ~System() = default;
  // `return_address` is the instruction after the software interrupt (bit 0
  // set = Thumb). Returns where execution continues: `return_address` when
  // the call has been dealt with here, or the address of code that deals with
  // it, after Machine::enter_exception; that code returns to
  // `return_address` (from translated code too: it is run until it does).
  virtual std::uint32_t software_interrupt(ds::Cpu& cpu, std::uint32_t number,
                                           std::uint32_t return_address) = 0;
  virtual std::uint32_t coprocessor_read(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2) = 0;
  virtual void coprocessor_write(ds::Cpu& cpu, int cp, int opc1, int crn, int crm, int opc2,
                                 std::uint32_t value) = 0;
  // Something that cannot be executed correctly. The run ends: see
  // Machine::stop.
  virtual void unhandled(ds::Cpu& cpu, std::uint32_t address, const char* what) = 0;
  // Where the processor's exception vectors are.
  virtual std::uint32_t exception_vectors() { return 0; }
  // The alarm set with Machine::set_alarm went off. The processor model is
  // not current here: do not read or change it.
  virtual void alarm() {}
};

struct Statistics {
  // How often the interpreter was entered, by the address it started at (bit
  // 0 set = Thumb). Each address is a gap in the translation.
  std::map<std::uint32_t, std::uint64_t> interpreter_entries;
  // The instructions interpreted from each of those addresses, until the
  // interpreter handed control back.
  std::map<std::uint32_t, std::uint64_t> interpreted_from;
  std::uint64_t interpreted_instructions = 0;
  // Instructions run by translated code, as it counts them (ds::Cpu::executed).
  std::uint64_t translated_instructions = 0;
  // Hand-backs from interpreted code to translated functions.
  std::uint64_t native_calls_from_interpreter = 0;
  std::uint64_t native_jumps_from_interpreter = 0;
  // Computed jumps that turned out to be returns to a waiting call site.
  std::uint64_t returns_through_jumps = 0;
  // Translated functions that returned somewhere other than to the call site
  // waiting for them.
  std::uint64_t returns_elsewhere = 0;
  // Call sites told to give up because control went to an outer one.
  std::uint64_t call_sites_abandoned = 0;
  // Interrupts the processor took, and of those, the ones taken in translated
  // code (where it polls for them).
  std::uint64_t interrupts_taken = 0;
  std::uint64_t interrupts_in_translated_code = 0;
  // Overlay translations compared with memory, and how many of those
  // comparisons found that memory held something else.
  std::uint64_t translations_checked = 0;
  std::uint64_t translations_rejected = 0;
  // Times the native frames of translated code were let go of because the
  // call sites waiting in them were nested too deep (see Machine).
  std::uint64_t nesting_dropped = 0;
};

// A place where guest code ran in the interpreter rather than natively: what
// the run log's "not native" report lists (Machine::gaps).
struct Gap {
  enum class Why {
    kNoTranslation,  // No translated function starts at this address.
    kOverlayOther,   // One does, for an overlay that was not in memory then.
    kCodeChanged,    // One does, for code always in memory, which the program has changed.
    kTranslatedNow,  // One does and is usable now (it was not when entered).
  };
  std::uint32_t address = 0;  // Bit 0 set = Thumb.
  std::uint64_t entries = 0;
  std::uint64_t instructions = 0;
  Why why = Why::kNoTranslation;
  bool readable = false;      // Whether there is code memory at the address now,
  std::uint32_t word = 0;     // and the instruction there (16 bits for Thumb).
};
const char* gap_reason(Gap::Why why);
// Where in the console's memory `address` is, in words.
const char* memory_area(std::uint32_t address);
// The "not native" report: a line for each gap, tab-separated, after a
// heading that says what the columns are. For the list of work to do.
std::string not_native_report(const std::vector<Gap>& gaps);

// Call sites that are never returned to. Some code uses a call instruction
// as a long jump (Thumb's "bl" reaches further than "b"), going round a loop
// of such "calls" that never return. Each one is a native call in the
// translated code, so the native stack would grow with every pass until it
// overflowed. When the call sites waiting reach kDeepestNesting, the
// machine therefore lets go of all of them: every native frame above the
// outermost run returns, and execution continues from the processor model
// where it was. Control that comes back to one of those call sites later is
// still followed, only no longer natively at once (a return to an address
// no call site waits for: see Statistics::returns_elsewhere).
class Machine {
 public:
  static constexpr std::uint32_t kDeepestNesting = 1024;

  // `table` ends with an entry whose function is null. All three arguments
  // must outlive the machine.
  Machine(Bus& bus, System& system, const TranslatedEntry* table);
  ~Machine();
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  ds::Cpu& cpu() { return cpu_; }
  // Returns the processor to its initial state.
  void reset_cpu();

  // Overlays. Several can be translated for one address range, though only
  // one is in memory at a time, and the game's own code does the loading. A
  // translation that belongs to an overlay is therefore used only while the
  // bytes it was made from are what guest memory holds; anything else at that
  // address is interpreted. This needs the overlay's image (`size` bytes that
  // load at `load_address`), which must outlive the machine. Translations of
  // overlays that were never added are not used.
  void add_overlay(int id, std::uint32_t load_address, const std::uint8_t* image, std::size_t size);

  // The program's code that is always in memory (overlay -1): a block of
  // `size` bytes at `load_address`, as it is once the start-up code has put
  // it there (the main program, and what it copies to the instruction
  // memory, for example). Once any is added, a translation of such code is
  // used only while the bytes it was made from are in memory, as for
  // overlays: a program can change its own code as it runs (the game turns
  // the first instruction of a function into a branch to another), and the
  // changed code is then interpreted. The image must outlive the machine.
  void add_program_code(std::uint32_t load_address, const std::uint8_t* image, std::size_t size);

  // Must be called after guest memory in an overlay's range is changed other
  // than through this machine (for example when a file is read into memory),
  // so that translations are checked against it again.
  void note_memory_written(std::uint32_t address, std::size_t size);

  // Calls the guest function at `address` (bit 0 set = Thumb) and returns when
  // control comes back to `return_address` with the stack pointer unchanged.
  // Arguments and results are exchanged through cpu().
  void call_function(std::uint32_t address, std::uint32_t return_address);

  // The interrupt request line. While it is set and the processor's
  // interrupt mask is clear, the processor takes the interrupt exception:
  // IRQ mode, and on to the vector. Interpreted code takes it before its
  // next instruction; translated code where it polls for it (ds::poll):
  // before going round a loop, after a write to the system coprocessor or to
  // the status register, before a call or jump through the runtime, and
  // after a BIOS call.
  void set_interrupt_line(bool on) {
    interrupt_line_ = on;
    cpu_.irq = on ? 1u : 0u;
  }
  bool interrupt_line() const { return interrupt_line_; }

  // Puts the processor model into exception mode `mode` the way the processor
  // enters an exception: the status register is saved for that mode,
  // interrupts are masked, and the mode's link register becomes `link`.
  // `thumb` is the instruction set being left. The caller sees to it that
  // execution continues at the vector.
  void enter_exception(std::uint32_t mode, std::uint32_t link, bool thumb);

  // Instructions executed so far, interpreted or translated (translated code
  // counts its own; see ds::Cpu::executed). What console time is made of.
  std::uint64_t executed() const { return executed_ + cpu_.executed; }

  // Calls System::alarm once `instructions` more have been executed. There is
  // one alarm; setting it again replaces it. In translated code, the alarm
  // goes off at the next memory access, call or other request to the
  // runtime: a loop that touches no memory runs on without it.
  void set_alarm(std::uint64_t instructions) { alarm_at_ = executed() + instructions; }

  // Ends the run: the outermost call_function returns as soon as it can. The
  // interpreter stops before its next instruction. Translated functions that
  // are in progress stop at their next call or return, not before, and what
  // they do until then no longer means anything. May be called from a System
  // handler. Also happens after System::unhandled.
  void stop();
  bool stopped() const { return stopped_; }

  // Whether a call or jump to `address` would run translated code right now.
  bool is_translated(std::uint32_t address) { return lookup(address) != nullptr; }

  // Where guest code was last entered through the runtime: a translated
  // function it called or jumped to, or the address the interpreter started
  // at. Translated functions calling one another directly do not change it.
  // For reports of a crash or a hang, so readable from any thread (and from
  // a signal handler: it is lock-free).
  const std::atomic<std::uint32_t>& last_entered() const { return last_entered_; }

  const Statistics& statistics() const { return statistics_; }
  // Every place the interpreter was entered at, the most instructions
  // interpreted first, with why it was not native.
  std::vector<Gap> gaps();
  // Whether any translated code was given (a game library is in use).
  bool has_translations() const { return table_.size() > 1; }
  void clear_statistics() { statistics_ = Statistics(); }

  // Stops interpretation, as unhandled, after this many instructions within
  // one call_function. 0 means no limit. For tests and traces: translated
  // code is not counted.
  void set_instruction_limit(std::uint64_t limit) { instruction_limit_ = limit; }
  // Whether the last call_function stopped for that reason.
  bool instruction_limit_reached() const { return limit_reached_; }

  // Entry points for the functions the translated code calls (ds::call,
  // ds::jump, ds::leave_slow, ds::interrupt, ds::swi). Not for other use.
  void dispatch_call(std::uint32_t address);
  void dispatch_jump(std::uint32_t address);
  bool leave_slow(std::uint32_t return_address);
  bool interrupt(std::uint32_t next);
  bool software_interrupt(std::uint32_t number, std::uint32_t return_address);
  // Adds what translated code has counted since (ds::Cpu::executed) to the
  // instructions executed, and sounds the alarm when it is due. Before every
  // request translated code makes of the runtime.
  void count_translated() {
    if (cpu_.executed == 0) return;
    executed_ += cpu_.executed;
    statistics_.translated_instructions += cpu_.executed;
    cpu_.executed = 0;
    if (executed_ >= alarm_at_) {
      alarm_at_ = ~std::uint64_t{0};
      system_.alarm();
    }
  }
  Bus& bus() { return bus_; }
  System& system() { return system_; }
  // Guest writes made by translated and interpreted code come through here.
  void watch_write(std::uint32_t address) {
    const std::uint32_t offset = address - watch_start_;
    if (offset < watch_size_) {
      ++page_generation_[offset >> kPageShift];
      if (code_pages_stale_ || !code_pages_[offset >> kCodeShift].empty()) mark_code_written(address, 4);
    }
  }

 private:
  using Function = void (*)(ds::Cpu&);
  static constexpr unsigned kPageShift = 12;
  // The code of the translations, in pieces of 1 << kCodeShift bytes over
  // the watched range: what a write must mark as changed.
  static constexpr unsigned kCodeShift = 8;
  struct CodeSpan {
    std::uint32_t start = 0, end = 0;  // Guest addresses, end exclusive.
    std::uint32_t entry = 0;           // In table_.
  };
  void mark_code_written(std::uint32_t address, std::size_t size);
  void make_code_pages();
  struct Overlay {
    std::uint32_t load_address = 0;
    const std::uint8_t* image = nullptr;
    std::size_t size = 0;
  };
  // What the last comparison of an overlay translation with memory found,
  // and the state of the memory it was made against.
  struct Check {
    std::uint64_t stamp = 0;  // 0: never compared.
    bool matches = false;
  };
  Function lookup(std::uint32_t address);
  bool in_memory(std::size_t index);
  std::uint64_t stamp_of(const TranslatedEntry& entry) const;
  int waiting_call_site(std::uint32_t address, std::uint32_t stack_pointer) const;
  void arrive(int call_site);
  void run_from(std::uint32_t address);
  bool interrupt_due() const { return interrupt_line_ && (cpu_.cpsr_ctrl & 0x80u) == 0; }
  // When the call sites waiting are nested kDeepestNesting deep above the
  // outermost run: arranges for all of them to give up and for the run the
  // giving up ends in (the outermost, or one at its depth entered through a
  // jump) to go on at `continue_at`, and returns true. The caller returns at
  // once, as after ds::leave returning true.
  bool drop_deep_nesting(std::uint32_t continue_at);
  // Runs from `address` (an exception vector) until control comes back to
  // `back_to` with stack pointer `stack_pointer`, as if a call site waited
  // there. Returns true when control went to an outer call site instead.
  bool run_until_back(std::uint32_t address, std::uint32_t back_to, std::uint32_t stack_pointer);
  bool interpret(std::uint32_t start, std::uint32_t* continue_at);
  void load_interpreter(std::uint32_t address, bd_arm_state* state);
  void store_from_interpreter(const bd_arm_state& state);
  void give_up(std::uint32_t address, const char* what);

  Bus& bus_;
  System& system_;
  std::vector<TranslatedEntry> table_;  // Sorted by address.
  std::vector<Check> checks_;           // Parallel to table_.
  std::map<int, std::vector<Overlay>> overlays_;  // By overlay; -1: the program's code always in memory.
  void watch_range(std::uint32_t load_address, std::size_t size);
  // Writes are counted per page over the range the overlays span. A
  // translation is compared with memory again only when a page holding its
  // code has been written since the last comparison.
  std::uint32_t watch_start_ = 0;
  std::uint32_t watch_size_ = 0;
  std::vector<std::uint64_t> page_generation_;
  std::vector<std::vector<CodeSpan>> code_pages_;
  bool code_pages_stale_ = false;
  bd_arm_core* core_ = nullptr;
  ds::Cpu cpu_;
  Statistics statistics_;
  std::uint64_t instruction_limit_ = 0;
  std::uint64_t instructions_this_call_ = 0;
  bool limit_reached_ = false;
  bool stopped_ = false;
  bool interrupt_line_ = false;
  std::uint64_t alarm_at_ = ~std::uint64_t{0};
  std::uint64_t executed_ = 0;  // Not counting cpu_.executed, which is added in.
  std::atomic<std::uint32_t> last_entered_{0};
  // run_from calls in progress, and the depth the outermost one began at.
  std::uint32_t runs_ = 0;
  std::uint32_t floor_depth_ = 0;
  // Set by drop_deep_nesting: where execution goes on once the call sites
  // above the outermost run have given up.
  bool resume_ = false;
  std::uint32_t resume_at_ = 0;
};

}  // namespace bd::runtime
