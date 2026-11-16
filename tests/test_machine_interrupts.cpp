// Interrupts and time in translated code (runtime/machine.h). The "translated"
// functions here are written by hand the way the translator writes them
// (tools/translator/dslab/arm_lift.py): counting their instructions, polling
// before going round a loop, returning through c.ret_to, and leaving an
// exception handler through ds::return_from_exception and ds::jump.
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

const TranslatedEntry kTable[] = {
    {0x00000008u, f_svc, -1, nullptr, 0},
    {0x00000018u, f_irq, -1, nullptr, 0},
    {0x00001000u, f_wait, -1, nullptr, 0},
    {0x00001100u, f_bios_call, -1, nullptr, 0},
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
