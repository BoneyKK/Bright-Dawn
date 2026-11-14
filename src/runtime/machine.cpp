#include "machine.h"

#include <algorithm>

#include "interp/arm_core.h"

namespace bd::runtime {
namespace {

using u32 = std::uint32_t;

constexpr u32 kThumbBit = 0x20;   // In the status register.
constexpr u32 kModeMask = 0x1F;
constexpr u32 kControlMask = 0xDF;  // Mode and interrupt masks; not the Thumb bit.

Machine& machine_of(void* user) { return *static_cast<Machine*>(user); }

// Memory and coprocessor access for the interpreter.
u32 host_read32(void* user, u32 a) { return machine_of(user).bus().read32(a); }
u32 host_read16(void* user, u32 a) { return machine_of(user).bus().read16(a); }
std::uint8_t host_read8(void* user, u32 a) { return machine_of(user).bus().read8(a); }
void host_write32(void* user, u32 a, u32 v) {
  Machine& m = machine_of(user);
  m.watch_write(a);
  m.bus().write32(a, v);
}
void host_write16(void* user, u32 a, std::uint16_t v) {
  Machine& m = machine_of(user);
  m.watch_write(a);
  m.bus().write16(a, v);
}
void host_write8(void* user, u32 a, std::uint8_t v) {
  Machine& m = machine_of(user);
  m.watch_write(a);
  m.bus().write8(a, v);
}
u32 host_cp_read(void* user, int cp, int opc1, int crn, int crm, int opc2) {
  Machine& m = machine_of(user);
  return m.system().coprocessor_read(m.cpu(), cp, opc1, crn, crm, opc2);
}
void host_cp_write(void* user, int cp, int opc1, int crn, int crm, int opc2, u32 value) {
  Machine& m = machine_of(user);
  m.system().coprocessor_write(m.cpu(), cp, opc1, crn, crm, opc2, value);
}

bool valid_mode(u32 mode) {
  switch (mode) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x17: case 0x1B: case 0x1F:
      return true;
    default:
      return false;
  }
}

bool mode_has_spsr(u32 status) {
  const u32 mode = status & kModeMask;
  return mode != 0x10 && mode != 0x1F;
}

// Whether an instruction with condition field `cond` executes under `status`.
bool condition_holds(u32 cond, u32 status) {
  const bool n = (status >> 31) & 1, z = (status >> 30) & 1, c = (status >> 29) & 1,
             v = (status >> 28) & 1;
  switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return true;
    default: return false;  // 0xF is not a condition; those encodings are other instructions.
  }
}

}  // namespace

Machine::Machine(Bus& bus, System& system, const TranslatedEntry* table)
    : bus_(bus), system_(system) {
  for (const TranslatedEntry* e = table; e->function != nullptr; ++e) table_.push_back(*e);
  std::stable_sort(table_.begin(), table_.end(),
                   [](const TranslatedEntry& a, const TranslatedEntry& b) { return a.address < b.address; });
  checks_.resize(table_.size());
  const bd_arm_host host = {this,         host_read32,  host_read16,  host_read8, host_write32,
                            host_write16, host_write8,  host_cp_read, host_cp_write};
  core_ = bd_arm_core_create(&host);
  reset_cpu();
}

Machine::~Machine() { bd_arm_core_destroy(core_); }

void Machine::reset_cpu() {
  // Not `cpu_ = ds::Cpu()`: that would copy the whole call-site list.
  for (u32& r : cpu_.r) r = 0;
  cpu_.N = cpu_.Z = cpu_.C = cpu_.V = cpu_.Q = false;
  cpu_.cpsr_ctrl = 0x1F;
  cpu_.spsr = 0;
  for (int i = 0; i < 6; ++i) cpu_.bank_r13[i] = cpu_.bank_r14[i] = cpu_.bank_spsr[i] = 0;
  for (auto& bank : cpu_.bank_r8)
    for (u32& r : bank) r = 0;
  cpu_.ret_to = 0;
  cpu_.cpu = 9;
  cpu_.platform = this;
  cpu_.depth = 0;
  cpu_.unwind = 0;
}

void Machine::add_overlay(int id, u32 load_address, const std::uint8_t* image, std::size_t size) {
  overlays_[id] = Overlay{load_address, image, size};
  // Watch writes over the whole range the overlays span.
  u32 start = load_address, end = load_address + static_cast<u32>(size);
  if (watch_size_ != 0) {
    start = std::min(start, watch_start_);
    end = std::max(end, watch_start_ + watch_size_);
  }
  watch_start_ = start & ~((u32{1} << kPageShift) - 1);
  watch_size_ = end - watch_start_;
  // Starting every page at a new generation makes all earlier comparisons stale.
  std::uint64_t highest = 0;
  for (std::uint64_t g : page_generation_) highest = std::max(highest, g);
  page_generation_.assign((watch_size_ >> kPageShift) + 1, highest + 1);
}

void Machine::note_memory_written(u32 address, std::size_t size) {
  if (size == 0 || watch_size_ == 0) return;
  const u32 last = address + static_cast<u32>(size - 1);
  for (u32 page = address >> kPageShift; page <= (last >> kPageShift); ++page) {
    const u32 offset = (page << kPageShift) - watch_start_;
    if ((page << kPageShift) >= watch_start_ && offset < watch_size_) ++page_generation_[offset >> kPageShift];
    if (page == 0xFFFFF) break;
  }
}

// A value that changes whenever a page holding the entry's code is written.
// Generations only grow, so their sum identifies the state of those pages.
std::uint64_t Machine::stamp_of(const TranslatedEntry& entry) const {
  std::uint64_t stamp = 0;
  for (u32 r = 0; r < entry.run_count; ++r) {
    const u32 first = entry.runs[r].address, last = first + entry.runs[r].length - 1;
    for (u32 page = first >> kPageShift; page <= (last >> kPageShift); ++page) {
      const u32 offset = (page << kPageShift) - watch_start_;
      if ((page << kPageShift) >= watch_start_ && offset < watch_size_) stamp += page_generation_[offset >> kPageShift];
    }
  }
  return stamp;
}

// Whether the code this overlay translation was made from is in memory.
bool Machine::in_memory(std::size_t index) {
  const TranslatedEntry& entry = table_[index];
  const auto found = overlays_.find(entry.overlay);
  if (found == overlays_.end() || entry.run_count == 0) return false;
  const Overlay& overlay = found->second;

  Check& check = checks_[index];
  const std::uint64_t stamp = stamp_of(entry);
  if (check.stamp == stamp) return check.matches;  // Nothing was written since.

  ++statistics_.translations_checked;
  bool matches = true;
  for (u32 r = 0; r < entry.run_count && matches; ++r) {
    const TranslatedRun& run = entry.runs[r];
    if (run.address < overlay.load_address || run.address - overlay.load_address > overlay.size ||
        run.length > overlay.size - (run.address - overlay.load_address)) {
      matches = false;  // The table and the image disagree; do not trust the translation.
      break;
    }
    const std::uint8_t* expected = overlay.image + (run.address - overlay.load_address);
    for (u32 i = 0; i < run.length; ++i) {
      if (bus_.read8(run.address + i) != expected[i]) {
        matches = false;
        break;
      }
    }
  }
  if (!matches) ++statistics_.translations_rejected;
  check.stamp = stamp;
  check.matches = matches;
  return matches;
}

Machine::Function Machine::lookup(u32 address) {
  auto it = std::lower_bound(
      table_.begin(), table_.end(), address,
      [](const TranslatedEntry& e, u32 a) { return e.address < a; });
  for (; it != table_.end() && it->address == address; ++it) {
    if (it->overlay < 0) return it->function;
    if (in_memory(static_cast<std::size_t>(it - table_.begin()))) return it->function;
  }
  return nullptr;
}

// The innermost call site that is waiting for control to come back to
// `address` with stack pointer `stack_pointer`, or -1. Both must match: the
// calling convention returns with the stack pointer the call was made with,
// and that tells one activation of a recursive call site from another.
int Machine::waiting_call_site(u32 address, u32 stack_pointer) const {
  const u32 recorded = std::min<u32>(cpu_.depth, ds::Cpu::kMaxFrames);
  for (u32 j = recorded; j-- > 0;) {
    if (cpu_.frames[j].ret == address && cpu_.frames[j].sp == stack_pointer) return static_cast<int>(j);
  }
  return -1;
}

// Control has reached what call site `call_site` is waiting for. Everything
// registered inside it must give up; ds::leave counts them down.
void Machine::arrive(int call_site) {
  cpu_.ret_to = cpu_.frames[call_site].ret;
  cpu_.unwind = (cpu_.depth - 1) - static_cast<u32>(call_site);
  statistics_.call_sites_abandoned += cpu_.unwind;
}

void Machine::give_up(u32 address, const char* what) {
  system_.unhandled(cpu_, address, what);
  stop();
}

void Machine::enter_exception(u32 mode, u32 link, bool thumb) {
  const u32 status = ds::get_cpsr(cpu_) | (thumb ? kThumbBit : 0u);
  ds::switch_mode(cpu_, mode);
  cpu_.spsr = status;
  cpu_.r[14] = link;
  cpu_.cpsr_ctrl |= 0x80u;
}

void Machine::stop() {
  stopped_ = true;
  // Every waiting call site gives up, the outermost one included.
  cpu_.unwind = cpu_.depth;
}

// Execution continues at `address`, by whatever means, until control arrives
// at a waiting call site.
void Machine::run_from(u32 address) {
  for (;;) {
    if (stopped_) return;
    if (const Function function = lookup(address)) {
      function(cpu_);
      if (cpu_.unwind != 0) return;  // Control went to an outer call site.
      // The function returned to cpu_.ret_to. Normally a call site waits there.
      const int site = waiting_call_site(cpu_.ret_to, cpu_.r[13]);
      if (site >= 0) {
        arrive(site);
        return;
      }
      // It went somewhere else: an exception return, for example.
      ++statistics_.returns_elsewhere;
      address = cpu_.ret_to;
      continue;
    }
    if (interpret(address, &address)) return;
    // The interpreter met a jump to a translated function; go round again.
    ++statistics_.native_jumps_from_interpreter;
  }
}

void Machine::dispatch_call(u32 address) { run_from(address); }

void Machine::dispatch_jump(u32 address) {
  // A jump to exactly where a call site is waiting is a return: for example
  // "pop {r3}; bx r3", which the translator does not recognise as one.
  const int site = waiting_call_site(address, cpu_.r[13]);
  if (site >= 0) {
    ++statistics_.returns_through_jumps;
    arrive(site);
    return;
  }
  run_from(address);
}

bool Machine::leave_slow(u32 return_address) {
  (void)return_address;
  if (cpu_.unwind != 0) {
    --cpu_.unwind;
    return true;
  }
  if (stopped_) return true;  // Stopped with nothing left to unwind: an outer call_function.
  // The callee returned, but to cpu_.ret_to and not to this call site: for
  // example it was called with "mov lr, pc; bx rX" further down, or it skips
  // data placed after the call. Continue from where it went. This call site
  // waits again meanwhile, so that control coming back here is recognised.
  ++statistics_.returns_elsewhere;
  ++cpu_.depth;
  const int site = waiting_call_site(cpu_.ret_to, cpu_.r[13]);
  if (site >= 0) {
    arrive(site);
  } else {
    run_from(cpu_.ret_to);
  }
  --cpu_.depth;
  if (cpu_.unwind == 0) return false;  // Control came back to this call site.
  --cpu_.unwind;
  return true;
}

void Machine::call_function(u32 address, u32 return_address) {
  if (cpu_.depth == 0) {  // Not a call made from inside another one.
    instructions_this_call_ = 0;
    limit_reached_ = false;
    stopped_ = false;
  }
  cpu_.r[14] = return_address;
  ds::enter(cpu_, return_address);
  dispatch_call(address);
  if (ds::leave(cpu_, return_address) && !stopped_)
    system_.unhandled(cpu_, address, "control left the called function without returning to its caller");
}

// Copies the processor model into the interpreter, to continue at `address`.
void Machine::load_interpreter(u32 address, bd_arm_state* state) {
  for (int i = 0; i < 15; ++i) state->r[i] = cpu_.r[i];
  state->r[15] = address & ~1u;
  u32 mode = cpu_.cpsr_ctrl & kModeMask;
  if (!valid_mode(mode)) mode = 0x1F;
  state->cpsr = (static_cast<u32>(cpu_.N) << 31) | (static_cast<u32>(cpu_.Z) << 30) |
                (static_cast<u32>(cpu_.C) << 29) | (static_cast<u32>(cpu_.V) << 28) |
                (static_cast<u32>(cpu_.Q) << 27) | (cpu_.cpsr_ctrl & 0xC0u) | mode |
                ((address & 1u) ? kThumbBit : 0u);
  state->spsr = cpu_.spsr;
  for (int i = 0; i < 6; ++i) {
    state->bank_r13[i] = cpu_.bank_r13[i];
    state->bank_r14[i] = cpu_.bank_r14[i];
    state->bank_spsr[i] = cpu_.bank_spsr[i];
  }
  for (int bank = 0; bank < 2; ++bank)
    for (int i = 0; i < 5; ++i) state->bank_r8[bank][i] = cpu_.bank_r8[bank][i];
  bd_arm_core_set_state(core_, state);
}

void Machine::store_from_interpreter(const bd_arm_state& state) {
  for (int i = 0; i < 15; ++i) cpu_.r[i] = state.r[i];
  cpu_.N = (state.cpsr >> 31) & 1;
  cpu_.Z = (state.cpsr >> 30) & 1;
  cpu_.C = (state.cpsr >> 29) & 1;
  cpu_.V = (state.cpsr >> 28) & 1;
  cpu_.Q = (state.cpsr >> 27) & 1;
  cpu_.cpsr_ctrl = (cpu_.cpsr_ctrl & ~kControlMask) | (state.cpsr & kControlMask);
  if (mode_has_spsr(state.cpsr)) cpu_.spsr = state.spsr;
  for (int i = 0; i < 6; ++i) {
    cpu_.bank_r13[i] = state.bank_r13[i];
    cpu_.bank_r14[i] = state.bank_r14[i];
    cpu_.bank_spsr[i] = state.bank_spsr[i];
  }
  for (int bank = 0; bank < 2; ++bank)
    for (int i = 0; i < 5; ++i) cpu_.bank_r8[bank][i] = state.bank_r8[bank][i];
}

// Interprets from `start` until control arrives at a waiting call site
// (returns true), or reaches a jump to a translated function (returns false
// with its address in *continue_at and the model up to date).
bool Machine::interpret(u32 start, u32* continue_at) {
  ++statistics_.interpreter_entries[start];
  if (core_ == nullptr) {
    give_up(start, "the interpreter could not be created");
    return true;
  }
  if (cpu_.depth > ds::Cpu::kMaxFrames) {
    give_up(start, "more nested calls than the runtime records");
    return true;
  }

  if (!bus_.has_code_memory(start & ~1u)) {
    give_up(start & ~1u, "execution reached an address where there is no memory");
    return true;
  }

  bd_arm_state state;
  load_interpreter(start, &state);
  for (;;) {
    const u32 pc = state.r[15];
    const bool thumb = (state.cpsr & kThumbBit) != 0;
    const u32 size = thumb ? 2 : 4;
    const u32 next = (pc + size) | (thumb ? 1u : 0u);  // As a return address.

    // An interrupt is taken between instructions. The link register gets the
    // address of the instruction that was about to run, plus 4, in either
    // instruction set; handlers return with "subs pc, lr, #4".
    if (interrupt_line_ && (state.cpsr & 0x80u) == 0) {
      store_from_interpreter(state);
      enter_exception(0x12, pc + 4, thumb);
      ++statistics_.interrupts_taken;
      load_interpreter(system_.exception_vectors() + 0x18, &state);
      continue;
    }

    // Software interrupts go to the runtime, exactly as from translated code.
    // The interpreter would jump to a BIOS that is not there.
    const u32 opcode = thumb ? bus_.read16(pc) : bus_.read32(pc);
    const bool is_swi = thumb ? (opcode & 0xFF00u) == 0xDF00u
                              : ((opcode & 0x0F000000u) == 0x0F000000u &&
                                 condition_holds(opcode >> 28, state.cpsr));
    if (is_swi) {
      store_from_interpreter(state);
      const u32 go = system_.software_interrupt(cpu_, thumb ? (opcode & 0xFFu) : ((opcode >> 16) & 0xFFu), next);
      if (stopped_) return true;
      load_interpreter(go, &state);
      continue;
    }

    if (instruction_limit_ != 0 && ++instructions_this_call_ > instruction_limit_) {
      store_from_interpreter(state);
      limit_reached_ = true;
      give_up(pc, "instruction limit reached while interpreting");
      return true;
    }
    const u32 mode_before = state.cpsr & kModeMask;
    bd_arm_state after;
    bd_arm_core_step(core_);
    bd_arm_core_get_state(core_, &after);
    ++statistics_.interpreted_instructions;
    if (statistics_.interpreted_instructions >= alarm_at_) {
      alarm_at_ = ~std::uint64_t{0};
      system_.alarm();
    }
    if (stopped_) {  // A handler called during the instruction ended the run.
      store_from_interpreter(after);
      return true;
    }

    // An undefined instruction: the interpreter took the exception. (Mode and
    // vector together, because code may also enter that mode on purpose.)
    if ((after.cpsr & kModeMask) == 0x1B && mode_before != 0x1B && after.r[15] == 0x00000004u) {
      store_from_interpreter(state);
      give_up(pc, "undefined instruction in interpreted code");
      return true;
    }
    if (!bd_arm_core_branched(core_)) {
      state = after;
      continue;
    }

    // The flow of control changed. Where to?
    const u32 target = after.r[15] | ((after.cpsr & kThumbBit) ? 1u : 0u);

    // 1. To where a translated call site is waiting: back to native code.
    const int site = waiting_call_site(target, after.r[13]);
    if (site >= 0) {
      store_from_interpreter(after);
      arrive(site);
      return true;
    }

    // 2. To a translated function: run it natively.
    if (const Function function = lookup(target)) {
      store_from_interpreter(after);
      if (after.r[14] != next) {
        // A jump. The caller decides what follows once the function returns.
        *continue_at = target;
        return false;
      }
      // A call: the instruction left its own return address in r14. The
      // interpreted code is the call site, so it registers like one.
      ++statistics_.native_calls_from_interpreter;
      ds::enter(cpu_, next);
      function(cpu_);
      if (ds::leave(cpu_, next)) return true;  // Control went to an outer call site.
      load_interpreter(next, &state);
      continue;
    }

    // 3. Somewhere with no translation: keep interpreting, if there is
    // anything there to interpret.
    if (!bus_.has_code_memory(after.r[15])) {
      store_from_interpreter(after);
      give_up(after.r[15], "execution reached an address where there is no memory");
      return true;
    }
    state = after;
  }
}

}  // namespace bd::runtime

// What the translated code calls.
namespace ds {
namespace {
bd::runtime::Machine& machine(Cpu& c) { return *static_cast<bd::runtime::Machine*>(c.platform); }
}  // namespace

u8 Cpu::rd8(u32 a) { return machine(*this).bus().read8(a); }
u16 Cpu::rd16(u32 a) { return machine(*this).bus().read16(a); }
u32 Cpu::rd32(u32 a) { return machine(*this).bus().read32(a); }
void Cpu::wr8(u32 a, u8 v) {
  bd::runtime::Machine& m = machine(*this);
  m.watch_write(a);
  m.bus().write8(a, v);
}
void Cpu::wr16(u32 a, u16 v) {
  bd::runtime::Machine& m = machine(*this);
  m.watch_write(a);
  m.bus().write16(a, v);
}
void Cpu::wr32(u32 a, u32 v) {
  bd::runtime::Machine& m = machine(*this);
  m.watch_write(a);
  m.bus().write32(a, v);
}

void call(Cpu& c, u32 address) { machine(c).dispatch_call(address); }
void jump(Cpu& c, u32 address) { machine(c).dispatch_jump(address); }
bool leave_slow(Cpu& c, u32 ret) { return machine(c).leave_slow(ret); }
void swi(Cpu& c, u32 number, u32 return_address) {
  bd::runtime::Machine& m = machine(c);
  const u32 go = m.system().software_interrupt(c, number, return_address);
  if (go != return_address && !m.stopped()) {
    // The call is dealt with by code at `go`, which translated code cannot
    // yet be left for and come back from.
    const u32 size = (return_address & 1u) ? 2 : 4;
    m.system().unhandled(c, (return_address & ~1u) - size,
                         "a BIOS call that runs as code was made from translated code");
    m.stop();
  }
}
void cp_write(Cpu& c, int cp, int opc1, int crn, int crm, int opc2, u32 value) {
  machine(c).system().coprocessor_write(c, cp, opc1, crn, crm, opc2, value);
}
u32 cp_read(Cpu& c, int cp, int opc1, int crn, int crm, int opc2) {
  return machine(c).system().coprocessor_read(c, cp, opc1, crn, crm, opc2);
}
void unhandled(Cpu& c, u32 address, const char* what) { machine(c).system().unhandled(c, address, what); }
}  // namespace ds
