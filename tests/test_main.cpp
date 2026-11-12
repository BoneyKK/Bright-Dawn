#include <cstring>
#include <exception>
#include <vector>

#include "test.h"

namespace bd_test {
namespace {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int g_failures_in_current_test = 0;

}  // namespace

void register_test(const char* name, void (*fn)()) { registry().push_back({name, fn}); }

void fail(const char* file, int line, const char* expression) {
  ++g_failures_in_current_test;
  std::printf("    %s:%d: check failed: %s\n", file, line, expression);
}

}  // namespace bd_test

// Usage: bd_tests [substring]   Runs the tests whose name contains substring.
int main(int argc, char** argv) {
  using namespace bd_test;
  const char* filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  int failed = 0;
  for (const TestCase& test : registry()) {
    if (std::strstr(test.name, filter) == nullptr) continue;
    g_failures_in_current_test = 0;
    try {
      test.fn();
    } catch (const Abort&) {
    } catch (const std::exception& e) {
      fail(__FILE__, __LINE__, e.what());
    }
    ++ran;
    if (g_failures_in_current_test != 0) ++failed;
    std::printf("[%s] %s\n", g_failures_in_current_test == 0 ? " ok " : "FAIL", test.name);
  }
  std::printf("%d test(s) run, %d failed\n", ran, failed);
  if (ran == 0) {
    std::printf("no tests matched\n");
    return 1;
  }
  return failed == 0 ? 0 : 1;
}
