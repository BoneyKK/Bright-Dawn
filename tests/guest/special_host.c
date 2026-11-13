// What the tests in special.S compute, in C. Host build only: this is the
// reference those assembly functions are compared with.
#include <stdint.h>

typedef uint32_t u32;

u32 a_leaf(u32 a, u32 b);
u32 t_leaf(u32 a, u32 b);

u32 a_handback(u32 a, u32 b) { return a_leaf(a + 1, b) ^ (a_leaf(b, a) + 7); }
u32 t_handback(u32 a, u32 b) { return a_handback(a, b); }

u32 a_retreg(u32 a, u32 b) { return a_leaf(a ^ 0x1234u, b) + 3; }
u32 t_retreg(u32 a, u32 b) { return t_leaf(a ^ 0x1234u, b) + 3; }

u32 a_v4call(u32 a, u32 b) { return a_leaf(a, b) + 5; }
u32 t_v4call(u32 a, u32 b) { return a_v4call(a, b); }

u32 a_inltab(u32 a, u32 b) {
  switch (a & 3u) {
    case 0: return b + 1;
    case 1: return b + b;
    case 2: return a ^ b;
    default: return 0u - b;
  }
}
u32 t_inltab(u32 a, u32 b) { return a_inltab(a, b); }

u32 a_nonlocal(u32 a, u32 b) { return (a & 1u) ? a + b + 7 : a + 101; }
u32 t_nonlocal(u32 a, u32 b) { return a_nonlocal(a, b); }

u32 a_skipdata(u32 a, u32 b) { return a + 0x1234u + b; }
u32 t_skipdata(u32 a, u32 b) { return a_skipdata(a, b); }

// Software interrupt 5 as the tests define it.
static u32 svc5(u32 r0, u32 r1) { return r0 * 3 + r1; }
u32 a_svc(u32 a, u32 b) { return svc5(svc5(a, b) + b, b); }
u32 t_svc(u32 a, u32 b) { return a_svc(a, b); }

// Three steps of the second flow, each replacing the value by leaf(value, b).
u32 a_coro(u32 a, u32 b) {
  u32 value = a;
  int step;
  for (step = 0; step < 3; ++step) value = a_leaf(value, b);
  return value;
}
u32 t_coro(u32 a, u32 b) { return a_coro(a, b); }

static u32 ror(u32 v, unsigned n) { return (v >> n) | (v << (32 - n)); }

// Values put into the registers of IRQ and Supervisor mode and read back,
// then System mode's sp and lr as set from IRQ mode.
u32 a_modes(u32 a, u32 b) {
  u32 r = (a + 0x100u) + ror(a ^ b, 7);
  r ^= (b & 0xF0000000u) | 0x1Fu;
  r += b + 0x200u;
  r ^= ror(a - b, 13);
  r += (a & 0xF0000000u) | 0x92u;
  r ^= a ^ 0x55AAu;
  r += b + 3;
  // System mode's r10 and r11, then FIQ mode's.
  r ^= ~a;
  r += b + 9;
  r += ((a + b) ^ ror(b, 9)) + a;
  return r;
}
u32 t_modes(u32 a, u32 b) { return a_modes(a, b); }

u32 a_hidmodes(u32 a, u32 b) { return ((a + 1) + ror(b ^ 0x77u, 3)) ^ (a + b); }
u32 t_hidmodes(u32 a, u32 b) { return a_hidmodes(a, b); }

// The status register after each return: the flags that were saved, and the
// mode and interrupt masks that were saved. The Thumb bit is gone again by
// the time the second one is read, in ARM code.
u32 a_excret(u32 a, u32 b) {
  u32 r = a + ((b & 0xF8000000u) | 0x5Fu);
  u32 loaded = r ^ b;
  u32 status = (r & 0xF8000000u) | 0x1Fu;
  return (r ^ ror(loaded, 5)) + status;
}
u32 t_excret(u32 a, u32 b) { return a_excret(a, b); }
