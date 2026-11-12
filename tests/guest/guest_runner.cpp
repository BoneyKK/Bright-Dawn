#include "guest_runner.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "inputs.h"
#define T(x) std::uint32_t a_##x(std::uint32_t, std::uint32_t); std::uint32_t t_##x(std::uint32_t, std::uint32_t);
#include "tests.def"
#undef T
}

namespace guest {
namespace {

using HostFunction = std::uint32_t (*)(std::uint32_t, std::uint32_t);
struct Test {
  const char* name;
  HostFunction host[2];  // The ARM-state build, then the Thumb-state build.
};
#define T(x) {#x, {a_##x, t_##x}},
const Test kTests[] = {
#include "tests.def"
};
#undef T

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  out.resize(kRamSize);
  out.resize(std::fread(out.data(), 1, out.size(), f));
  std::fclose(f);
  return true;
}

}  // namespace

void Memory::load(const std::vector<std::uint8_t>& image) {
  std::memset(bytes_.data(), 0, bytes_.size());
  std::memcpy(bytes_.data(), image.data(), image.size());
  faulted_ = false;
}

std::uint8_t* Memory::at(std::uint32_t address, std::uint32_t size) {
  if (address < kRamBase || address - kRamBase > kRamSize - size) {
    faulted_ = true;
    std::memset(scratch_, 0, sizeof scratch_);
    return scratch_;
  }
  return bytes_.data() + (address - kRamBase);
}

int run_all(Executor& executor, Memory& memory, const char* image_path,
            const std::map<std::string, Expect>& expectations) {
  std::vector<std::uint8_t> image;
  if (!read_file(image_path, image) || image.size() < sizeof kTests / sizeof kTests[0] * 8 + 4) {
    std::printf("cannot read the guest image %s\n", image_path);
    return 2;
  }
  for (const auto& [name, expect] : expectations) {
    bool known = false;
    for (const Test& test : kTests) known = known || name == test.name;
    if (!known) {
      std::printf("expectation given for a test that does not exist: %s\n", name.c_str());
      return 2;
    }
    (void)expect;
  }

  std::printf("executor: %s\n", executor.name());
  int failures = 0;
  int index = 0;
  for (const Test& test : kTests) {
    const auto found = expectations.find(test.name);
    const Expect expect = found == expectations.end() ? Expect::Pass : found->second;
    for (int variant = 0; variant < 2; ++variant, ++index) {
      std::uint32_t address;
      std::memcpy(&address, image.data() + index * 4, 4);
      memory.load(image);  // A fresh image for every test.

      long wrong = 0, gaps = 0, failed = 0;
      std::map<std::string, long> details;
      guest_rng rng = guest_rng_seed();
      for (int i = 0; i < GUEST_PAIR_COUNT; ++i) {
        std::uint32_t a, b;
        guest_input(&rng, i, &a, &b);
        const CallResult got = executor.call(memory, address, a, b);
        const std::uint32_t want = test.host[variant](a, b);
        if (memory.faulted()) {
          ++failed;
          ++details["touched memory outside the guest RAM"];
          memory.clear_fault();
        } else if (got.outcome == Outcome::Gap) {
          ++gaps;
          ++details[got.detail];
        } else if (got.outcome == Outcome::Failed) {
          ++failed;
          ++details[got.detail];
        } else if (got.result != want) {
          if (wrong++ == 0) {
            char text[96];
            std::snprintf(text, sizeof text, "f(%08x, %08x) gave %08x, reference %08x",
                          static_cast<unsigned>(a), static_cast<unsigned>(b),
                          static_cast<unsigned>(got.result), static_cast<unsigned>(want));
            ++details[text];
          }
        } else if (got.stack_pointer != kStackTop) {
          ++failed;
          ++details["returned with the stack pointer moved"];
        }
      }

      const char* verdict = "ok";
      if (wrong != 0 || failed != 0) {
        verdict = "FAIL";
      } else if (expect == Expect::Pass && gaps != 0) {
        verdict = "FAIL (unexpected gap)";
      } else if (expect == Expect::Gap && gaps == 0) {
        // Keep the expectations honest: a gap that has been closed must be
        // removed from the list, so it cannot quietly reopen.
        verdict = "FAIL (expected a gap, but every input completed)";
      } else if (expect == Expect::Gap) {
        verdict = "ok (known gap)";
      }
      if (std::strncmp(verdict, "FAIL", 4) == 0) ++failures;
      std::printf("  %-5s %-9s %-22s %d inputs, %ld wrong, %ld stopped at a gap, %ld failed\n",
                  variant == 0 ? "arm" : "thumb", test.name, verdict, GUEST_PAIR_COUNT, wrong, gaps,
                  failed);
      int shown = 0;
      for (const auto& [what, count] : details) {
        if (shown++ == 6) {
          std::printf("        ... and %zu more kinds\n", details.size() - 6);
          break;
        }
        std::printf("        %ld x %s\n", count, what.c_str());
      }
    }
  }
  std::printf("%d of %d failed\n", failures, index);
  return failures == 0 ? 0 : 1;
}

}  // namespace guest
