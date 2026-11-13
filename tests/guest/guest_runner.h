// Shared by the tests that run the ARM build of the guest corpus through an
// executor of DS code and compare every result with the host build.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace guest {

inline constexpr std::uint32_t kRamBase = 0x02000000u;
inline constexpr std::uint32_t kRamSize = 0x00100000u;
inline constexpr std::uint32_t kStackTop = kRamBase + kRamSize - 0x100u;
// Return address handed to the guest function. Nothing is stored there.
inline constexpr std::uint32_t kReturnAddress = kRamBase + kRamSize - 0x40u;

// Flat guest memory. Accesses outside it are recorded, not performed.
class Memory {
 public:
  Memory() : bytes_(kRamSize) {}
  void load(const std::vector<std::uint8_t>& image);
  std::uint8_t* at(std::uint32_t address, std::uint32_t size);
  bool faulted() const { return faulted_; }
  void clear_fault() { faulted_ = false; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint8_t scratch_[8] = {};
  bool faulted_ = false;
};

enum class Outcome {
  Completed,  // The function returned; `result` holds r0.
  Gap,        // Execution reached something the executor does not cover.
  Failed,     // The executor misbehaved: ran away, or touched memory outside RAM.
};

struct CallResult {
  Outcome outcome = Outcome::Failed;
  std::uint32_t result = 0;
  std::uint32_t stack_pointer = 0;
  std::string detail;  // For Gap and Failed: what happened.
};

class Executor {
 public:
  virtual ~Executor() = default;
  virtual const char* name() const = 0;
  // Calls the guest function at `address` (bit 0 set = Thumb) with two
  // arguments, stack pointer kStackTop and return address kReturnAddress.
  virtual CallResult call(Memory& memory, std::uint32_t address, std::uint32_t a,
                          std::uint32_t b) = 0;
  // Called around the inputs of one test in one instruction set. end_test may
  // return a complaint, which fails the test; empty means none.
  virtual void begin_test(const std::string& name, bool thumb) {
    (void)name;
    (void)thumb;
  }
  virtual std::string end_test(const std::string& name, bool thumb) {
    (void)name;
    (void)thumb;
    return std::string();
  }
};

// The one software interrupt the corpus uses, as every test platform must
// implement it: number 5 computes r0 = r0 * 3 + r1.
inline constexpr std::uint32_t kTestSoftwareInterrupt = 5;
inline std::uint32_t test_software_interrupt(std::uint32_t r0, std::uint32_t r1) { return r0 * 3 + r1; }

// Per-test expectation.
enum class Expect {
  Pass,  // Every input must give the reference result.
  Gap,   // No input may give a wrong result, and at least one must hit a gap.
};

// Runs every test in both instruction sets on the fixed inputs. `expectations`
// maps test names (as in tests.def) to Expect::Gap; all others must pass.
// `memory` is the guest memory the executor works on; it is reloaded with the
// image before each test. Prints one line per test and returns the process
// exit status.
int run_all(Executor& executor, Memory& memory, const char* image_path,
            const std::map<std::string, Expect>& expectations);

}  // namespace guest
