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

#include "guest_bus.h"
#include "guest_runner.h"
#include "machine.h"

extern "C" {
#include "inputs.h"
}

namespace {

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
    return check_counters(test, s);
  }

  // The assembly tests are each built around one mechanism, so the number of
  // times that mechanism fires per input is known. Checking it shows that
  // the right path was taken, not merely that the answer was right.
  static std::string check_counters(const std::string& test, const bd::runtime::Statistics& s) {
    const std::uint64_t n = GUEST_PAIR_COUNT;
    struct Expected {
      const char* test;
      std::uint64_t native_calls, native_jumps, returns_through_jumps, returns_elsewhere, abandoned;
    };
    // Per input: hand-back calls, hand-back jumps, jumps that were returns,
    // calls that returned elsewhere, call sites abandoned.
    static const Expected kExpected[] = {
        {"handback", 1, 1, 0, 0, 0},  // One hidden helper calls, the other jumps.
        {"retreg", 0, 0, 1, 0, 0},    // The return through a register.
        {"v4call", 0, 0, 0, 1, 0},    // The callee returns into its caller, where no call site waits.
        {"inltab", 0, 0, 0, 0, 1},    // The caller of the helper is abandoned.
        {"skipdata", 0, 0, 0, 1, 1},  // The helper returns past the data word.
        {"svc", 0, 0, 0, 0, 0},
        {"coro", 6, 0, 0, 6, 3},      // Three steps, each switching there and back.
        {"hidmodes", 0, 0, 0, 0, 0},  // The hidden code is called and returns; nothing unusual.
        // Two exception returns, each leaving a translated function for somewhere
        // other than its caller. The second function was called from interpreted
        // code, and that call site is abandoned when the test finally returns.
        {"excret", 1, 0, 0, 2, 1},
    };
    for (const Expected& e : kExpected) {
      if (test != e.test) continue;
      if (s.native_calls_from_interpreter != e.native_calls * n) return "unexpected number of calls handed back to translated code";
      if (s.native_jumps_from_interpreter != e.native_jumps * n) return "unexpected number of jumps handed back to translated code";
      if (s.returns_through_jumps != e.returns_through_jumps * n) return "unexpected number of jumps recognised as returns";
      if (s.returns_elsewhere != e.returns_elsewhere * n) return "unexpected number of calls that returned elsewhere";
      if (s.call_sites_abandoned != e.abandoned * n) return "unexpected number of abandoned call sites";
    }
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
  guest::Bus bus_;
  guest::System system_;
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
