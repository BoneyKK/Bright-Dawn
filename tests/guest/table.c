// Function table at the very start of the ARM image: for every test, the
// address of its ARM build and then of its Thumb build (which has bit 0 set),
// ended by a zero. Whatever runs the image finds the functions through this.
#include <stdint.h>
typedef uint32_t (*fn2)(uint32_t, uint32_t);
#define T(x) extern uint32_t a_##x(uint32_t, uint32_t); extern uint32_t t_##x(uint32_t, uint32_t);
#include "tests.def"
#undef T
#define T(x) a_##x, t_##x,
__attribute__((section(".table"), used)) const fn2 test_table[] = {
#include "tests.def"
  0 };
