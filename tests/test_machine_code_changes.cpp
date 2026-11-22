// Translated code that the program changes as it runs (runtime/machine.h,
// Machine::add_program_code). The "translated" functions here are written by
// hand the way the translator writes them (tools/translator/dslab/arm_lift.py):
// each tests its mark first, and one calls the other directly, as translated
// code does, without the runtime looking it up.
#include <array>
#include <cstdint>

#include "machine.h"
#include "test.h"

namespace {

using ds::u32;

constexpr u32 kReturn = 0x2000;  // Where the test's call returns to.
constexpr u32 kCallee = 0x4000;
constexpr u32 kCaller = 0x4100;
constexpr u32 kMovR0_1 = 0xE3A00001;  // mov r0, #1
constexpr u32 kMovR0_2 = 0xE3A00002;  // mov r0, #2
constexpr u32 kBxLr = 0xE12FFF1E;     // bx lr

class Memory : public bd::runtime::Bus {
 public:
  std::uint8_t read8(u32 a) override { return bytes_[a & 0xFFFF]; }
  std::uint16_t read16(u32 a) override { return static_cast<std::uint16_t>(read8(a) | (read8(a + 1) << 8)); }
  u32 read32(u32 a) override { return read16(a) | (static_cast<u32>(read16(a + 2)) << 16); }
  void write8(u32 a, std::uint8_t v) override { bytes_[a & 0xFFFF] = v; }
  void write16(u32 a, std::uint16_t v) override {
    write8(a, static_cast<std::uint8_t>(v));
    write8(a + 1, static_cast<std::uint8_t>(v >> 8));
  }
  void write32(u32 a, u32 v) override {
    write16(a, static_cast<std::uint16_t>(v));
    write16(a + 2, static_cast<std::uint16_t>(v >> 16));
  }

 private:
  std::array<std::uint8_t, 0x10000> bytes_{};
};

class Hardware : public bd::runtime::System {
 public:
  u32 software_interrupt(ds::Cpu&, u32, u32 return_address) override { return return_address; }
  u32 coprocessor_read(ds::Cpu&, int, int, int, int, int) override { return 0; }
  void coprocessor_write(ds::Cpu&, int, int, int, int, int, u32) override {}
  void unhandled(ds::Cpu&, u32, const char*) override { ++unhandled_count; }
  void alarm() override {}
  int unhandled_count = 0;
};

int g_callee_runs = 0;  // Times the translation of the callee ran.

// 0x4000, the callee:
//   4000 mov r0, #1
//   4004 bx lr
ds::u8 f_callee_changed = 0;
void f_callee(ds::Cpu& c) {
  if (f_callee_changed != 0) {
    ds::jump(c, kCallee);
    return;
  }
  ++g_callee_runs;
  c.executed += 2;
  c.r[0] = 1;
  c.ret_to = c.r[14];
}

// 0x4100, the caller, which calls the callee directly:
//   4100 mov r4, lr
//   4104 bl 4000
//   4108 bx r4
ds::u8 f_caller_changed = 0;
void f_caller(ds::Cpu& c) {
  if (f_caller_changed != 0) {
    ds::jump(c, kCaller);
    return;
  }
  c.executed += 2;
  c.r[4] = c.r[14];
  c.r[14] = 0x4108;
  ds::enter(c, 0x4108);
  f_callee(c);
  if (ds::leave(c, 0x4108)) return;
  c.executed += 1;
  c.ret_to = c.r[4];
}

const TranslatedRun kCalleeRun[] = {{kCallee, 8}};
const TranslatedRun kCallerRun[] = {{kCaller, 12}};
const TranslatedEntry kTable[] = {
    {kCallee, f_callee, -1, kCalleeRun, 1, &f_callee_changed},
    {kCaller, f_caller, -1, kCallerRun, 1, &f_caller_changed},
    {0, nullptr, 0, nullptr, 0},
};

// The program as it was translated, from 0x4000.
std::array<std::uint8_t, 0x200> program_image() {
  std::array<std::uint8_t, 0x200> image{};
  const auto put = [&](u32 address, u32 word) {
    for (u32 i = 0; i < 4; ++i) image[address - kCallee + i] = static_cast<std::uint8_t>(word >> (8 * i));
  };
  put(kCallee, kMovR0_1);
  put(kCallee + 4, kBxLr);
  put(kCaller, 0xE1A0400E);      // mov r4, lr
  put(kCaller + 4, 0xEBFFFFBD);  // bl 4000
  put(kCaller + 8, 0xE12FFF14);  // bx r4
  return image;
}

struct Rig {
  std::array<std::uint8_t, 0x200> image = program_image();
  Memory memory;
  Hardware hardware;
  bd::runtime::Machine machine{memory, hardware, kTable};
  Rig() {
    for (u32 i = 0; i < image.size(); ++i) memory.write8(kCallee + i, image[i]);
    machine.add_program_code(kCallee, image.data(), image.size());
    machine.note_memory_written(kCallee, image.size());  // As the console does when it loads the program.
    ds::Cpu& c = machine.cpu();
    c.r[13] = 0x7000;
    c.cpsr_ctrl = 0x1F;
    g_callee_runs = 0;
  }
  u32 call() {
    machine.call_function(kCaller, kReturn);
    return machine.cpu().r[0];
  }
};

}  // namespace

TEST(translated_code_runs_while_the_program_s_code_is_as_translated) {
  Rig rig;
  CHECK(rig.call() == 1);
  CHECK(g_callee_runs == 1 && rig.hardware.unhandled_count == 0 && !rig.machine.stopped());
}

TEST(code_the_program_changes_is_interpreted_even_when_called_directly) {
  Rig rig;
  CHECK(rig.call() == 1);
  // The program changes the callee's first instruction, as a translated
  // store does: the translation no longer applies, though the caller calls
  // it directly.
  rig.machine.cpu().wr32(kCallee, kMovR0_2);
  CHECK(f_callee_changed != 0);
  CHECK(rig.call() == 2);
  CHECK(g_callee_runs == 1 && rig.hardware.unhandled_count == 0);
  // The place is listed with why.
  bool listed = false;
  for (const bd::runtime::Gap& g : rig.machine.gaps())
    if (g.address == kCallee) listed = g.why == bd::runtime::Gap::Why::kCodeChanged;
  CHECK(listed);
  // Changed back, the translation runs again.
  rig.machine.cpu().wr32(kCallee, kMovR0_1);
  CHECK(rig.call() == 1);
  CHECK(g_callee_runs == 2 && f_callee_changed == 0);
}

TEST(a_write_next_to_translated_code_leaves_it_alone) {
  Rig rig;
  CHECK(rig.call() == 1);  // Loading the program marked both; the first call checked them.
  CHECK(f_callee_changed == 0 && f_caller_changed == 0);
  rig.machine.cpu().wr32(kCallee + 8, 0x12345678);  // Past the callee's code: data.
  CHECK(f_callee_changed == 0 && f_caller_changed == 0);
  CHECK(rig.call() == 1 && g_callee_runs == 2);
}
