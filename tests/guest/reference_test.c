// Checks the host build of the guest corpus against recorded checksums.
//
// The host build is the reference that executors of DS code are compared
// with, so it must give the same answers under every compiler. Run with
// --print to produce the table for golden.def after changing the corpus.
#include <stdio.h>
#include <string.h>

#include "inputs.h"

#define T(x) uint32_t a_##x(uint32_t, uint32_t); uint32_t t_##x(uint32_t, uint32_t);
#include "tests.def"
#undef T

typedef uint32_t (*fn2)(uint32_t, uint32_t);
typedef struct {
  const char* name;
  fn2 first;   /* built as the ARM-state variant */
  fn2 second;  /* built as the Thumb-state variant */
  uint32_t golden;
} test_case;

#define G(name, hash) hash,
static const uint32_t golden[] = {
#include "golden.def"
    0 /* end marker, so the table is never empty */
};
#undef G

#define T(x) {#x, a_##x, t_##x, 0},
static test_case tests[] = {
#include "tests.def"
};
#undef T

static uint32_t checksum(fn2 fn) {
  guest_rng rng = guest_rng_seed();
  uint32_t hash = GUEST_HASH_START;
  int i;
  for (i = 0; i < GUEST_PAIR_COUNT; ++i) {
    uint32_t a, b;
    guest_input(&rng, i, &a, &b);
    hash = guest_hash_step(hash, fn(a, b));
  }
  return hash;
}

int main(int argc, char** argv) {
  const int count = (int)(sizeof tests / sizeof tests[0]);
  const int print = argc > 1 && strcmp(argv[1], "--print") == 0;
  int failed = 0;
  int i;
  const int recorded = (int)(sizeof golden / sizeof golden[0]) - 1;  /* less the end marker */
  if (!print && recorded != count) {
    printf("golden.def has %d entries but tests.def has %d tests\n", recorded, count);
    return 1;
  }
  for (i = 0; i < count; ++i) {
    const uint32_t first = checksum(tests[i].first);
    const uint32_t second = checksum(tests[i].second);
    if (print) {
      printf("G(%s, 0x%08Xu)\n", tests[i].name, (unsigned)first);
    }
    if (first != second) {
      printf("FAIL %s: the two host builds disagree (%08X, %08X)\n", tests[i].name, (unsigned)first,
             (unsigned)second);
      ++failed;
    } else if (!print && first != golden[i]) {
      printf("FAIL %s: checksum %08X, recorded %08X\n", tests[i].name, (unsigned)first,
             (unsigned)golden[i]);
      ++failed;
    } else if (!print) {
      printf("ok   %s\n", tests[i].name);
    }
  }
  if (!print) printf("%d test(s), %d failed, %d input pairs each\n", count, failed, GUEST_PAIR_COUNT);
  return failed ? 1 : 0;
}
