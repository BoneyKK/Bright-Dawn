// A deliberately small test harness: enough for this project's needs without
// bringing in an external dependency.
#pragma once

#include <cstdio>

namespace bd_test {

struct Abort {};

void register_test(const char* name, void (*fn)());
void fail(const char* file, int line, const char* expression);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { register_test(name, fn); }
};

}  // namespace bd_test

#define TEST(name)                                                       \
  static void bd_test_fn_##name();                                       \
  static const ::bd_test::Registrar bd_test_reg_##name(#name, &bd_test_fn_##name); \
  static void bd_test_fn_##name()

// Records a failure and continues with the test.
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) ::bd_test::fail(__FILE__, __LINE__, #cond);       \
  } while (0)

// Records a failure and abandons the test; use when later lines would crash.
#define REQUIRE(cond)                                              \
  do {                                                             \
    if (!(cond)) {                                                 \
      ::bd_test::fail(__FILE__, __LINE__, #cond);                  \
      throw ::bd_test::Abort{};                                    \
    }                                                              \
  } while (0)
