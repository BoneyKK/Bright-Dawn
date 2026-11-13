// Runs the guest corpus the way the game will run: translated code, with the
// interpreter behind it for whatever has no translation.
//
// Every test must give the reference result. Beyond that, this checks the rule
// that translated code is preferred: a test that the translator covers
// completely must never enter the interpreter.
//
// usage: guest_mixed_test <guest image> [test that may use the interpreter]...
#include <cstdio>
#include <cstring>
#include <set>

#include "guest_runner.h"
#include "machine.h"

namespace {

class GuestBus : public bd::runtime::Bus {
 public:
  explicit GuestBus(guest::Memory& memory) : memory_(memory) {}
  std::uint8_t read8(std::uint32_t a) override { return *memory_.at(a, 1); }
  std::uint16_t read16(std::uint32_t a) override { std::uint16_t v; std::memcpy(&v, memory_.at(a, 2), 2); return v; }
  std::uint32_t read32(std::uint32_t a) override { std::uint32_t v; std::memcpy(&v, memory_.at(a, 4), 4); return v; }
  void write8(std::uint32_t a, std::uint8_t v) override { *memory_.at(a, 1) = v; }
  void write16(std::uint32_t a, std::uint16_t v) override { std::memcpy(memory_.at(a, 2), &v, 2); }
  void write32(std::uint32_t a, std::uint32_t v) override { std::memcpy(memory_.at(a, 4), &v, 4); }

 private:
  guest::Memory& memory_;
};

// The corpus uses nothing outside the processor, so any such request is a fault.
class GuestSystem : public bd::runtime::System {
 public:
  void software_interrupt(ds::Cpu& cpu, std::uint32_t number) override {
    if (number == guest::kTestSoftwareInterrupt) {
      cpu.r[0] = guest::test_software_interrupt(cpu.r[0], cpu.r[1]);
    } else {
      note("software interrupt %u", number);
    }
  }
  std::uint32_t coprocessor_read(ds::Cpu&, int, int, int, int, int) override { note("coprocessor read%.0u", 0); return 0; }
  void coprocessor_write(ds::Cpu&, int, int, int, int, int, std::uint32_t) override { note("coprocessor write%.0u", 0); }
  void unhandled(ds::Cpu&, std::uint32_t address, const char* what) override {
    char text[160];
    std::snprintf(text, sizeof text, "unhandled at %08x: %s", static_cast<unsigned>(address), what);
    if (problem.empty()) problem = text;
  }
  std::string problem;

 private:
  void note(const char* format, std::uint32_t value) {
    char text[64];
    std::snprintf(text, sizeof text, format, static_cast<unsigned>(value));
    if (problem.empty()) problem = text;
  }
};

class MixedExecutor : public guest::Executor {
 public:
  MixedExecutor(guest::Memory& memory, std::set<std::string> may_interpret)
      : bus_(memory), machine_(bus_, system_, translated_table), may_interpret_(std::move(may_interpret)) {
    machine_.set_instruction_limit(2000000);
  }

  const char* name() const override { return "translated code with the interpreter behind it"; }

  void begin_test(const std::string&, bool) override { machine_.clear_statistics(); }

  std::string end_test(const std::string& test, bool) override {
    const bd::runtime::Statistics& s = machine_.statistics();
    total_.interpreted_instructions += s.interpreted_instructions;
    total_.native_calls_from_interpreter += s.native_calls_from_interpreter;
    total_.native_jumps_from_interpreter += s.native_jumps_from_interpreter;
    total_.returns_through_jumps += s.returns_through_jumps;
    total_.returns_elsewhere += s.returns_elsewhere;
    total_.call_sites_abandoned += s.call_sites_abandoned;
    for (const auto& [address, count] : s.interpreter_entries) {
      total_.interpreter_entries[address] += count;
      // The interpreter must never be started on something that has a translation.
      if (machine_.is_translated(address)) return "the interpreter was entered at a translated function";
    }
    const bool allowed = may_interpret_.count(test) != 0;
    if (!allowed && !s.interpreter_entries.empty())
      return "a fully translated test entered the interpreter";
    if (allowed && s.interpreter_entries.empty())
      return "expected to need the interpreter, but did not: remove it from the list";
    return std::string();
  }

  guest::CallResult call(guest::Memory&, std::uint32_t address, std::uint32_t a, std::uint32_t b) override {
    machine_.reset_cpu();
    ds::Cpu& c = machine_.cpu();
    c.r[0] = a;
    c.r[1] = b;
    c.r[13] = guest::kStackTop;
    system_.problem.clear();
    machine_.call_function(address, guest::kReturnAddress);
    guest::CallResult out;
    if (!system_.problem.empty()) {
      out.detail = system_.problem;
    } else if (c.depth != 0 || c.unwind != 0) {
      out.detail = "the list of waiting call sites was not empty afterwards";
    } else {
      out.outcome = guest::Outcome::Completed;
      out.result = c.r[0];
      out.stack_pointer = c.r[13];
    }
    return out;
  }

  const bd::runtime::Statistics& total() const { return total_; }

 private:
  GuestBus bus_;
  GuestSystem system_;
  bd::runtime::Machine machine_;
  std::set<std::string> may_interpret_;
  bd::runtime::Statistics total_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: guest_mixed_test <guest image> [test that may use the interpreter]...\n");
    return 2;
  }
  std::set<std::string> may_interpret;
  for (int i = 2; i < argc; ++i) may_interpret.insert(argv[i]);
  guest::Memory memory;
  MixedExecutor executor(memory, may_interpret);
  const int status = guest::run_all(executor, memory, argv[1], {});
  const bd::runtime::Statistics& t = executor.total();
  std::printf("interpreter: entered at %zu distinct addresses, %llu instructions interpreted\n",
              t.interpreter_entries.size(), static_cast<unsigned long long>(t.interpreted_instructions));
  std::printf("  handed back to translated code: %llu calls, %llu jumps\n",
              static_cast<unsigned long long>(t.native_calls_from_interpreter),
              static_cast<unsigned long long>(t.native_jumps_from_interpreter));
  std::printf("  jumps that were returns: %llu; calls that returned elsewhere: %llu; call sites abandoned: %llu\n",
              static_cast<unsigned long long>(t.returns_through_jumps),
              static_cast<unsigned long long>(t.returns_elsewhere),
              static_cast<unsigned long long>(t.call_sites_abandoned));
  return status;
}
