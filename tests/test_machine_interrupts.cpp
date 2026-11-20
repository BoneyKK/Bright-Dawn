// Interrupts and time in translated code (runtime/machine.h). The "translated"
// functions here are written by hand the way the translator writes them
// (tools/translator/dslab/arm_lift.py): counting their instructions, polling
// before going round a loop, returning through c.ret_to, and leaving an
// exception handler through ds::return_from_exception and ds::jump.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "machine.h"
#include "test.h"

namespace {

using ds::u32;

constexpr u32 kAcknowledge = 0x8000;  // A write here clears the interrupt request.
constexpr u32 kFlag = 0x8004;         // What the handler leaves for the program.
constexpr u32 kReturn = 0x2000;       // Where the test's call returns to.

class Memory : public bd::runtime::Bus {
 public:
  bd::runtime::Machine* machine = nullptr;
  std::uint8_t read8(u32 a) override { return bytes_[a & 0xFFFF]; }
  std::uint16_t read16(u32 a) override { return static_cast<std::uint16_t>(read8(a) | (read8(a + 1) << 8)); }
  u32 read32(u32 a) override { return read16(a) | (static_cast<u32>(read16(a + 2)) << 16); }
  void write8(u32 a, std::uint8_t v) override { bytes_[a & 0xFFFF] = v; }
  void write16(u32 a, std::uint16_t v) override {
    write8(a, static_cast<std::uint8_t>(v));
    write8(a + 1, static_cast<std::uint8_t>(v >> 8));
  }
  void write32(u32 a, u32 v) override {
    if (a == kAcknowledge && machine != nullptr) machine->set_interrupt_line(false);
    write16(a, static_cast<std::uint16_t>(v));
    write16(a + 2, static_cast<std::uint16_t>(v >> 16));
  }

 private:
  std::array<std::uint8_t, 0x10000> bytes_{};
};

class Hardware : public bd::runtime::System {
 public:
  bd::runtime::Machine* machine = nullptr;
  int alarms = 0;
  u32 software_interrupts = 0;
  u32 software_interrupt(ds::Cpu&, u32 number, u32 return_address) override {
    ++software_interrupts;
    if (number != 4) return return_address;
    // As the console does for the BIOS calls that wait: the call runs as
    // code, at the vector, in Supervisor mode.
    machine->enter_exception(0x13, return_address & ~1u, (return_address & 1u) != 0);
    return 0x08;
  }
  u32 coprocessor_read(ds::Cpu&, int, int, int, int, int) override { return 0; }
  void coprocessor_write(ds::Cpu&, int, int, int, int, int, u32) override {}
  void unhandled(ds::Cpu&, u32, const char*) override { ++unhandled_count; }
  void alarm() override {
    ++alarms;
    machine->set_interrupt_line(true);  // A device asks for the processor.
  }
  int unhandled_count = 0;
};

// 0x1000: waits for the interrupt handler to set the flag.
//   1000 mov r0, #0
//   1004 ldr r0, [flag]   <- loop
//   1008 cmp r0, #0
//   100c beq 1004
//   1010 bx lr
void f_wait(ds::Cpu& c) {
  c.executed += 1;
  c.r[0] = 0;
L_00001004:
  c.executed += 4;
  c.r[0] = c.rd32(kFlag);
  c.Z = c.r[0] == 0;
  if (c.Z) {
    if (ds::poll(c, 0x00001004u)) return;
    goto L_00001004;
  }
  c.ret_to = c.r[14];
}

// 0x18, the interrupt vector: acknowledges, leaves its link register in the
// flag, and returns from the exception ("subs pc, lr, #4").
void f_irq(ds::Cpu& c) {
  c.executed += 4;
  c.wr32(kAcknowledge, 1);
  c.wr32(kFlag, c.r[14]);
  c.r[13] -= 8;  // Its own stack, used and given back.
  c.r[13] += 8;
  const u32 t = ds::return_from_exception(c, c.r[14] - 4);
  ds::jump(c, t);
}

// 0x1100: a BIOS call that runs as code, then returns what it left in r0.
//   1100 swi 4
//   1104 bx lr
void f_bios_call(ds::Cpu& c) {
  c.executed += 2;
  if (ds::swi(c, 4u, 0x00001104u)) return;
  c.ret_to = c.r[14];
}

// 0x08, the software interrupt vector: "the BIOS", which answers 42 and
// returns from the exception ("movs pc, lr").
void f_svc(ds::Cpu& c) {
  c.executed += 2;
  c.r[0] = 42;
  const u32 t = ds::return_from_exception(c, c.r[14]);
  ds::jump(c, t);
}

// 0x3000 and 0x3100: a loop made of calls that never return, as code that
// uses "bl" as a long jump has. 0x3000 calls 0x3100 directly, which calls
// 0x3000 back through the runtime (as a computed call would), and so on,
// with the stack pointer unchanged; after kLaps passes, 0x3000 jumps to
// where the test's call returns to.
constexpr u32 kLaps = 5000;
constexpr u32 kLap = 0x8008;  // The pass counter.
u32 g_deepest = 0;            // The most call sites waiting, seen from 0x3000.

void f_far_b(ds::Cpu& c);

void f_far_a(ds::Cpu& c) {
  c.executed += 4;
  g_deepest = std::max(g_deepest, c.depth);
  const u32 lap = c.rd32(kLap) + 1;
  c.wr32(kLap, lap);
  if (lap == kLaps) {
    ds::jump(c, kReturn);
    return;
  }
  c.r[14] = 0x00003011u;
  ds::enter(c, 0x00003011u);
  f_far_b(c);
  if (ds::leave(c, 0x00003011u)) return;
  ds::unhandled(c, 0x3010, "a call that never returns returned");
}

void f_far_b(ds::Cpu& c) {
  c.executed += 1;
  c.r[14] = 0x00003105u;
  ds::enter(c, 0x00003105u);
  ds::call(c, 0x00003000u);
  if (ds::leave(c, 0x00003105u)) return;
  ds::unhandled(c, 0x3104, "a call that never returns returned");
}

// 0x3200: jumps to 0x3000 through the runtime (a computed jump), so the
// loop of calls runs in a run nested at the same depth as the outermost.
void f_far_start(ds::Cpu& c) {
  c.executed += 1;
  ds::jump(c, 0x00003000u);
}

const TranslatedEntry kTable[] = {
    {0x00000008u, f_svc, -1, nullptr, 0},
    {0x00000018u, f_irq, -1, nullptr, 0},
    {0x00001000u, f_wait, -1, nullptr, 0},
    {0x00001100u, f_bios_call, -1, nullptr, 0},
    {0x00003000u, f_far_a, -1, nullptr, 0},
    {0x00003100u, f_far_b, -1, nullptr, 0},
    {0x00003200u, f_far_start, -1, nullptr, 0},
    {0, nullptr, 0, nullptr, 0},
};

struct Rig {
  Memory memory;
  Hardware hardware;
  bd::runtime::Machine machine{memory, hardware, kTable};
  Rig() {
    memory.machine = &machine;
    hardware.machine = &machine;
    ds::Cpu& c = machine.cpu();
    ds::switch_mode(c, 0x12);
    c.r[13] = 0x6000;  // IRQ mode's stack.
    ds::switch_mode(c, 0x13);
    c.r[13] = 0x5000;  // Supervisor mode's.
    ds::switch_mode(c, 0x1F);
    c.r[13] = 0x7000;  // System mode, interrupts unmasked.
    c.cpsr_ctrl = 0x1F;
  }
};

}  // namespace

TEST(translated_code_takes_an_interrupt_in_its_loop) {
  Rig rig;
  rig.machine.set_alarm(50);
  rig.machine.call_function(0x1000, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.hardware.alarms == 1);
  // The handler saw the link register the processor gives: the instruction
  // about to run, the loop's first, plus 4.
  CHECK(c.r[0] == 0x1008);
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F);  // Back in System mode,
  CHECK((c.cpsr_ctrl & 0x80u) == 0);     // with interrupts unmasked again,
  CHECK(c.r[13] == 0x7000);              // and its own stack.
  CHECK(!rig.machine.interrupt_line());
  const bd::runtime::Statistics& s = rig.machine.statistics();
  CHECK(s.interrupts_taken == 1 && s.interrupts_in_translated_code == 1);
  CHECK(s.interpreted_instructions == 0);
  // Time passed with the translated code: the alarm went off after 50.
  CHECK(s.translated_instructions >= 50);
  CHECK(rig.machine.executed() == s.translated_instructions);
  CHECK(rig.machine.cpu().depth == 0);
}

TEST(translated_code_does_not_take_a_masked_interrupt) {
  Rig rig;
  ds::Cpu& c = rig.machine.cpu();
  rig.machine.set_interrupt_line(true);
  c.cpsr_ctrl |= 0x80u;
  CHECK(!ds::poll(c, 0x1004));
  c.cpsr_ctrl &= ~0x80u;
  rig.machine.set_interrupt_line(false);
  CHECK(!ds::poll(c, 0x1004));
  CHECK(rig.machine.statistics().interrupts_taken == 0);
}

TEST(translated_code_runs_a_bios_call_that_is_code) {
  Rig rig;
  rig.machine.call_function(0x1100, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.hardware.software_interrupts == 1);
  CHECK(c.r[0] == 42);
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F);
  CHECK(c.r[13] == 0x7000);
  CHECK(c.depth == 0);
}

TEST(an_interrupt_comes_before_a_call_through_the_runtime) {
  Rig rig;
  rig.machine.set_interrupt_line(true);
  // The call goes through the runtime (ds::call); the interrupt is taken
  // before the called function's first instruction.
  rig.machine.call_function(0x1000, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.machine.statistics().interrupts_taken == 1);
  // The handler's flag was set before the loop first read it.
  CHECK(c.r[0] == 0x1004);
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F && c.r[13] == 0x7000);
}

TEST(calls_that_never_return_do_not_nest_without_end) {
  Rig rig;
  g_deepest = 0;
  rig.machine.call_function(0x3000, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.memory.read32(kLap) == kLaps);
  // Each pass leaves two call sites waiting; they are let go of before
  // there are too many, and the loop goes on where it was.
  CHECK(g_deepest <= bd::runtime::Machine::kDeepestNesting + 2);
  CHECK(rig.machine.statistics().nesting_dropped >= 2 * kLaps / bd::runtime::Machine::kDeepestNesting - 1);
  CHECK(c.depth == 0);
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F && c.r[13] == 0x7000);
}

TEST(deep_nesting_below_interpreted_code_is_let_go_of_too) {
  Rig rig;
  g_deepest = 0;
  // 0x3800, with no translation: "bl 0x3000", which the interpreter runs
  // as a call into the translated code, where the call sites pile up. Once
  // they are let go of, the run goes on from the outermost run.
  rig.memory.write32(0x3800, 0xEBFFFDFEu);
  rig.machine.call_function(0x3800, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.memory.read32(kLap) == kLaps);
  CHECK(rig.machine.statistics().native_calls_from_interpreter == 1);
  CHECK(rig.machine.statistics().nesting_dropped >= 2 * kLaps / bd::runtime::Machine::kDeepestNesting - 1);
  CHECK(g_deepest <= bd::runtime::Machine::kDeepestNesting + 3);
  CHECK(c.depth == 0);
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F && c.r[13] == 0x7000);
}

TEST(deep_nesting_below_a_jump_is_let_go_of_too) {
  Rig rig;
  g_deepest = 0;
  rig.machine.call_function(0x3200, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.memory.read32(kLap) == kLaps);
  CHECK(rig.machine.statistics().nesting_dropped >= 2 * kLaps / bd::runtime::Machine::kDeepestNesting - 1);
  CHECK(rig.machine.statistics().returns_elsewhere == 0);
  CHECK(g_deepest <= bd::runtime::Machine::kDeepestNesting + 2);
  CHECK(c.depth == 0);
}

TEST(an_interrupt_in_deep_nesting_is_still_taken) {
  Rig rig;
  g_deepest = 0;
  // The alarm raises the interrupt line partway, while call sites pile up.
  rig.machine.set_alarm(3000);
  rig.machine.call_function(0x3000, kReturn);
  CHECK(!rig.machine.stopped());
  CHECK(rig.hardware.unhandled_count == 0);
  CHECK(rig.hardware.alarms == 1);
  CHECK(rig.memory.read32(kLap) == kLaps);
  CHECK(rig.machine.statistics().interrupts_taken == 1);
  CHECK(!rig.machine.interrupt_line());
  CHECK(g_deepest <= bd::runtime::Machine::kDeepestNesting + 2);
  CHECK(rig.machine.cpu().depth == 0);
}

// Code with no translation, at 0x4000: "mov r0, #5; bx lr".
TEST(the_places_run_in_the_interpreter_are_listed_with_why) {
  Rig rig;
  rig.memory.write32(0x4000, 0xE3A00005);
  rig.memory.write32(0x4004, 0xE12FFF1E);
  rig.machine.call_function(0x4000, kReturn);
  rig.machine.call_function(0x4000, kReturn);
  CHECK(rig.machine.cpu().r[0] == 5 && rig.hardware.unhandled_count == 0);
  CHECK(rig.machine.has_translations());
  const std::vector<bd::runtime::Gap> gaps = rig.machine.gaps();
  REQUIRE(gaps.size() == 1);
  CHECK(gaps[0].address == 0x4000 && gaps[0].entries == 2 && gaps[0].instructions == 4);
  CHECK(gaps[0].why == bd::runtime::Gap::Why::kNoTranslation);
  CHECK(gaps[0].readable && gaps[0].word == 0xE3A00005);
  const std::string report = bd::runtime::not_native_report(gaps);
  CHECK(report.find("# 1 places, entered 2 times, 4 instructions interpreted.") != std::string::npos);
  CHECK(report.find("0x00004000\tARM\t2\t4\t0xE3A00005\tfast code memory\tno translated code starts here\n") !=
        std::string::npos);
}

// Interpreted code at 0x4100 waits for the handler's flag:
//   4100 ldr r0, [r1]
//   4104 cmp r0, #0
//   4108 beq 4100
//   410c bx lr
// The interrupt comes after two instructions, in the interpreter, which
// hands it to the translated vector instead of interpreting what is there.
TEST(the_interpreter_takes_an_interrupt_through_the_translated_vector) {
  Rig rig;
  rig.memory.write32(0x4100, 0xE5910000);
  rig.memory.write32(0x4104, 0xE3500000);
  rig.memory.write32(0x4108, 0x0AFFFFFC);
  rig.memory.write32(0x410C, 0xE12FFF1E);
  rig.machine.cpu().r[1] = kFlag;
  rig.machine.set_alarm(2);
  rig.machine.call_function(0x4100, kReturn);
  const ds::Cpu& c = rig.machine.cpu();
  CHECK(rig.hardware.unhandled_count == 0 && !rig.machine.stopped());
  CHECK(rig.machine.statistics().interrupts_taken == 1);
  CHECK(c.r[0] == 0x410C);  // The flag: the handler's link register, "beq" + 4.
  CHECK((c.cpsr_ctrl & 0x1Fu) == 0x1F && c.r[13] == 0x7000);
  // Only the loop was interpreted: two instructions, the handler, then
  // "beq" and once round.
  CHECK(rig.machine.statistics().interpreted_instructions == 7);
  CHECK(rig.machine.statistics().native_jumps_from_interpreter == 1);  // To the vector.
}
