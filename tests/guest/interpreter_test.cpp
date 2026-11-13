// Runs the guest corpus through the interpreter alone, from each function's
// first instruction to its return. This is the evidence that the interpreter
// executes ARMv5TE code correctly, independent of the translator.
//
// usage: guest_interpreter_test <guest image> [test expected to stop at a gap]...
#include <cstdio>
#include <cstring>

#include "arm_core.h"
#include "guest_runner.h"

namespace {

guest::Memory& memory_of(void* user) { return *static_cast<guest::Memory*>(user); }

std::uint32_t read32(void* u, std::uint32_t a) { std::uint32_t v; std::memcpy(&v, memory_of(u).at(a, 4), 4); return v; }
std::uint32_t read16(void* u, std::uint32_t a) { std::uint16_t v; std::memcpy(&v, memory_of(u).at(a, 2), 2); return v; }
std::uint8_t read8(void* u, std::uint32_t a) { return *memory_of(u).at(a, 1); }
void write32(void* u, std::uint32_t a, std::uint32_t v) { std::memcpy(memory_of(u).at(a, 4), &v, 4); }
void write16(void* u, std::uint32_t a, std::uint16_t v) { std::memcpy(memory_of(u).at(a, 2), &v, 2); }
void write8(void* u, std::uint32_t a, std::uint8_t v) { *memory_of(u).at(a, 1) = v; }
bool g_coprocessor_used;
std::uint32_t cp_read(void*, int, int, int, int, int) { g_coprocessor_used = true; return 0; }
void cp_write(void*, int, int, int, int, int, std::uint32_t) { g_coprocessor_used = true; }

class InterpreterExecutor : public guest::Executor {
 public:
  // One interpreter serves every call: creating one builds its decode tables.
  explicit InterpreterExecutor(guest::Memory& memory) {
    const bd_arm_host host = {&memory, read32, read16, read8, write32, write16, write8, cp_read, cp_write};
    core_ = bd_arm_core_create(&host);
  }
  ~InterpreterExecutor() override { bd_arm_core_destroy(core_); }
  InterpreterExecutor(const InterpreterExecutor&) = delete;
  InterpreterExecutor& operator=(const InterpreterExecutor&) = delete;

  const char* name() const override { return bd_arm_core_name(); }

  guest::CallResult call(guest::Memory&, std::uint32_t address, std::uint32_t a,
                         std::uint32_t b) override {
    bd_arm_core* core = core_;
    guest::CallResult out;
    if (core == nullptr) {
      out.detail = "out of memory";
      return out;
    }
    bd_arm_state state = {};
    state.r[0] = a;
    state.r[1] = b;
    state.r[13] = guest::kStackTop;
    state.r[14] = guest::kReturnAddress;
    state.r[15] = address & ~1u;
    state.cpsr = 0x1Fu | ((address & 1u) ? 0x20u : 0u);  // System mode
    bd_arm_core_set_state(core, &state);
    g_coprocessor_used = false;

    // The corpus functions run a few thousand instructions; this limit only
    // exists to catch an interpreter that never returns.
    const long kStepLimit = 2000000;
    long steps = 0;
    for (; steps < kStepLimit; ++steps) {
      bd_arm_core_step(core);
      bd_arm_core_get_state(core, &state);
      if (state.r[15] == guest::kReturnAddress) break;
      if ((state.cpsr & 0x1Fu) != 0x1Fu) break;  // An exception was taken.
    }
    if (steps == kStepLimit) {
      out.detail = "did not return";
    } else if ((state.cpsr & 0x1Fu) != 0x1Fu) {
      // On its own the interpreter sends software interrupts to a BIOS that
      // is not there. The runtime intercepts them; here it is a known gap.
      out.outcome = guest::Outcome::Gap;
      out.detail = "took an exception (undefined instruction or software interrupt)";
    } else if (g_coprocessor_used) {
      out.detail = "used the coprocessor";
    } else if ((state.cpsr & 0x20u) != 0) {
      out.detail = "returned in Thumb state to an ARM-state caller";
    } else {
      out.outcome = guest::Outcome::Completed;
      out.result = state.r[0];
      out.stack_pointer = state.r[13];
    }
    return out;
  }

 private:
  bd_arm_core* core_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: guest_interpreter_test <guest image> [known gap]...\n");
    return 2;
  }
  std::map<std::string, guest::Expect> expectations;
  for (int i = 2; i < argc; ++i) expectations[argv[i]] = guest::Expect::Gap;
  guest::Memory memory;
  InterpreterExecutor executor(memory);
  return guest::run_all(executor, memory, argv[1], expectations);
}
