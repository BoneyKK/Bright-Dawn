// Runs the guest corpus as C++ produced by the translator, with nothing behind
// it: a call to an address that has no translation is a gap. This measures
// the translator on its own.
//
// usage: guest_translated_test <guest image> [test name expected to hit a gap]...
#include <cstdio>
#include <cstring>

#include "ds_cpu.h"
#include "guest_runner.h"
#include "translated_table.h"

namespace {

// What stopped the current call, if anything.
std::string g_gap;

void note_gap(const char* format, ds::u32 value, const char* text = "") {
  if (!g_gap.empty()) return;  // Keep the first cause.
  char buffer[128];
  std::snprintf(buffer, sizeof buffer, format, static_cast<unsigned>(value), text);
  g_gap = buffer;
}

// After a gap the processor state means nothing, and running on with it can
// loop for ever. Tell every waiting call site to give up, which makes each
// translated function return as soon as its current call comes back.
void abandon(ds::Cpu& c) { c.unwind = c.depth; }

guest::Memory& memory_of(ds::Cpu& c) { return *static_cast<guest::Memory*>(c.platform); }

}  // namespace

// The platform side of the translated code: plain guest memory.
namespace ds {
u8 Cpu::rd8(u32 a) { return *memory_of(*this).at(a, 1); }
u16 Cpu::rd16(u32 a) { u16 v; std::memcpy(&v, memory_of(*this).at(a, 2), 2); return v; }
u32 Cpu::rd32(u32 a) { u32 v; std::memcpy(&v, memory_of(*this).at(a, 4), 4); return v; }
void Cpu::wr8(u32 a, u8 v) { *memory_of(*this).at(a, 1) = v; }
void Cpu::wr16(u32 a, u16 v) { std::memcpy(memory_of(*this).at(a, 2), &v, 2); }
void Cpu::wr32(u32 a, u32 v) { std::memcpy(memory_of(*this).at(a, 4), &v, 4); }

void call(Cpu& c, u32 address) {
  for (const TranslatedEntry* e = translated_table; e->function != nullptr; ++e) {
    if (e->address == address) {
      e->function(c);
      return;
    }
  }
  note_gap("no translation for %08x", address);
  abandon(c);
}
// Without a runtime behind it, a computed jump can only be a tail call to a
// translated function, and a call can only return to its own call site.
void jump(Cpu& c, u32 address) { call(c, address); }
bool leave_slow(Cpu& c, u32 ret) {
  if (c.unwind == 0) {
    note_gap("a call returned somewhere other than to %08x", ret);
    abandon(c);
    return true;
  }
  --c.unwind;
  return true;
}
void swi(Cpu& c, u32 number, u32) {
  if (number == guest::kTestSoftwareInterrupt) {
    c.r[0] = guest::test_software_interrupt(c.r[0], c.r[1]);
  } else {
    note_gap("software interrupt %u", number);
  }
}
void cp_write(Cpu&, int, int, int, int, int, u32) { note_gap("coprocessor write%.0u", 0); }
u32 cp_read(Cpu&, int, int, int, int, int) { note_gap("coprocessor read%.0u", 0); return 0; }
void unhandled(Cpu&, u32 address, const char* what) { note_gap("not translated at %08x: %s", address, what); }
}  // namespace ds

namespace {

class TranslatedExecutor : public guest::Executor {
 public:
  const char* name() const override { return "translated code only"; }
  guest::CallResult call(guest::Memory& memory, std::uint32_t address, std::uint32_t a,
                         std::uint32_t b) override {
    ds::Cpu& c = cpu_;
    c = ds::Cpu();
    c.platform = &memory;
    c.r[0] = a;
    c.r[1] = b;
    c.r[13] = guest::kStackTop;
    c.r[14] = guest::kReturnAddress;
    g_gap.clear();
    ds::enter(c, guest::kReturnAddress);
    ds::call(c, address);
    (void)ds::leave(c, guest::kReturnAddress);
    guest::CallResult out;
    out.result = c.r[0];
    out.stack_pointer = c.r[13];
    out.outcome = g_gap.empty() ? guest::Outcome::Completed : guest::Outcome::Gap;
    out.detail = g_gap;
    return out;
  }

 private:
  ds::Cpu cpu_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: guest_translated_test <guest image> [known gap]...\n");
    return 2;
  }
  std::map<std::string, guest::Expect> expectations;
  for (int i = 2; i < argc; ++i) expectations[argv[i]] = guest::Expect::Gap;
  guest::Memory memory;
  TranslatedExecutor executor;
  return guest::run_all(executor, memory, argv[1], expectations);
}
