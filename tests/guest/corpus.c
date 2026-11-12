// Guest test corpus: small functions that are compiled three ways - for the
// host (the reference), for ARMv5TE in ARM state, and for ARMv5TE in Thumb
// state - so that anything which executes DS code (translated code, an
// interpreter) can be checked against known-good results.
//
// Rules for everything in this file: a function maps two 32-bit inputs to one
// 32-bit result, depends on nothing but its inputs, and avoids behaviour that
// C leaves undefined or that differs between compilers. The reference test
// pins the results with checksums, so a host compiler that disagrees is caught.
#include <stdint.h>

#ifdef MODE_T
#define NAME(x) t_##x
#define OTHER(x) a_##x
#else
#define NAME(x) a_##x
#define OTHER(x) t_##x
#endif
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef uint32_t (*fn2)(u32, u32);

// Bit counting. On ARM the compiler's own forms are used, so that the ARM
// build contains the CLZ instruction; the host uses plain loops.
#if defined(__arm__)
static u32 count_lz(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static u32 count_tz(u32 v) { return v ? (u32)__builtin_ctz(v) : 32; }
static u32 count_ones(u32 v) { return (u32)__builtin_popcount(v); }
#else
static u32 count_lz(u32 v) { u32 n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static u32 count_tz(u32 v) { u32 n = 0; if (!v) return 32; while (!(v & 1u)) { v >>= 1; n++; } return n; }
static u32 count_ones(u32 v) { u32 n = 0; while (v) { n += v & 1u; v >>= 1; } return n; }
#endif

// Defined in the build of the other instruction set: calls cross between ARM and Thumb.
u32 OTHER(leaf)(u32 a, u32 b);
u32 OTHER(fib)(u32 n, u32 unused);

static u32 rotl(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

NOINLINE u32 NAME(leaf)(u32 a, u32 b) { return rotl(a ^ 0x9E3779B9u, b) + (b | 1) * 2654435761u; }

NOINLINE u32 NAME(arith)(u32 a, u32 b) {
  u32 r = a + b; r ^= a - b; r += b - a; r ^= (a & b) | (~a & 0x0F0F0F0Fu); r += (a | b) - (a ^ b);
  u64 w = (u64)a + b; r ^= (u32)(w >> 32); w = (u64)a - b; r += (u32)(w >> 32);
  return r - (a < b) + (a == b) * 7 + ((s32)a < (s32)b) * 13 + ((s32)a >= (s32)b) * 17 + (a >= b) * 19;
}

NOINLINE u32 NAME(shifts)(u32 a, u32 b) {
  u32 n = b & 31, r = a << n; r ^= a >> n; r += (u32)((s32)a >> n); r ^= rotl(a, b >> 5);
  r += (a << 1) ^ (a >> 31) ^ (u32)((s32)a >> 31) ^ (a >> 1) ^ (a << 31);
  r ^= (b >> ((a >> 3) & 31)) + (b << ((a >> 9) & 31));
  return r;
}

NOINLINE u32 NAME(mul)(u32 a, u32 b) {
  u32 r = a * b; r += a * 100003u + b; u64 u = (u64)a * b; s64 s = (s64)(s32)a * (s32)b;
  r ^= (u32)(u >> 32) ^ (u32)u; r += (u32)((u64)s >> 32) ^ (u32)s;
  u64 acc = ((u64)b << 32) | a; acc += (u64)a * b; s64 sacc = (s64)(((u64)a << 32) | b) + (s64)(s32)b * (s32)a;
  return r ^ (u32)(acc >> 32) ^ (u32)acc ^ (u32)((u64)sacc >> 32) ^ (u32)sacc;
}

NOINLINE u32 NAME(dsp)(u32 a, u32 b) {           // 16x16 multiplies and saturating adds
  s32 x = (int16_t)a, y = (int16_t)b, xh = (int16_t)(a >> 16), yh = (int16_t)(b >> 16);
  u32 r = (u32)(x * y) ^ (u32)(xh * y) ^ ((u32)(x * yh) + (u32)(xh * yh));
  r += (u32)(((s64)(s32)a * y) >> 16) ^ (u32)(((s64)(s32)b * xh) >> 16);
  r += b + (u32)(x * yh);
  s64 t = (s64)(s32)a + (s64)(s32)b; u32 q = t > 0x7FFFFFFF ? 0x7FFFFFFFu : t < -0x80000000LL ? 0x80000000u : (u32)t;
  s64 d = (s64)(s32)a - (s64)(s32)b; u32 p = d > 0x7FFFFFFF ? 0x7FFFFFFFu : d < -0x80000000LL ? 0x80000000u : (u32)d;
  return r ^ q ^ rotl(p, 7);
}

NOINLINE u32 NAME(clz)(u32 a, u32 b) {
  u32 r = count_lz(a); r = r * 33 + count_lz(b);
  r = r * 33 + count_ones(a) + count_tz(a);
  return r;
}

NOINLINE u32 NAME(loops)(u32 a, u32 b) {
  u32 crc = ~a;
  for (int i = 0; i < 4; i++) { crc ^= (b >> (i * 8)) & 0xFF; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  u32 x = a | 1, y = b | 1, steps = 0;
  while (x != y && steps < 200) { if (x > y) x -= y; else y -= x; steps++; }
  return ~crc ^ (x * 31 + steps);
}

static u32 buf32[64]; static uint16_t buf16[64]; static uint8_t buf8[64]; static u64 buf64[8];
struct rec { uint8_t a; int8_t b; uint16_t c; int16_t d; u32 e; u64 f; };
static struct rec recs[4];

NOINLINE u32 NAME(memory)(u32 a, u32 b) {
  for (u32 i = 0; i < 64; i++) { buf32[i] = a + i * b; buf16[i] = (uint16_t)(a >> (i & 15)) + (uint16_t)i; buf8[i] = (uint8_t)(b >> (i & 7)) ^ (uint8_t)i; }
  u32 r = 0;
  for (u32 i = 0; i < 64; i++) {
    r = r * 31 + buf32[(i * 7) & 63];
    r ^= ((u32)(s32)(int16_t)buf16[i]) + buf16[63 - i];
      r += ((u32)(s32)(int8_t)buf8[i]) ^ buf8[(i + a) & 63];
  }
  for (u32 i = 0; i < 8; i++) buf64[i] = ((u64)a << 32 | b) * (i + 1) + ((u64)b << 13);
  for (u32 i = 0; i < 8; i++) { u64 v = buf64[7 - i]; r ^= (u32)v + (u32)(v >> 32) * 3; }
  for (u32 i = 0; i < 4; i++) { struct rec *p = &recs[(a + i) & 3]; p->a = (uint8_t)(a >> i); p->b = (int8_t)(b >> i); p->c = (uint16_t)(a ^ b); p->d = (int16_t)(b - i); p->e = a * i; p->f = (u64)b << (i * 8); }
  for (u32 i = 0; i < 4; i++) { struct rec *p = &recs[i]; r += p->a + (u32)(s32)p->b + p->c + (u32)(s32)p->d + p->e + (u32)(p->f >> 16); }
  return r;
}

NOINLINE u32 NAME(stack)(u32 a, u32 b) {          // locals on the stack, addressed through pointers
  volatile u32 v[24];
  for (u32 i = 0; i < 24; i++) v[i] = a * (i + 1) ^ rotl(b, i);
  u32 r = 0;
  for (u32 i = 0; i < 24; i++) r += v[(i * 5) % 24] ^ (r >> 3);
  return r;
}

NOINLINE u32 NAME(fib)(u32 n, u32 unused) { (void)unused; n &= 15; return n < 2 ? n : NAME(fib)(n - 1, 0) + OTHER(fib)(n - 2, 0); }

NOINLINE static u32 NAME(many)(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u64 h) {
  return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + (u32)(h >> 32) * 19 + (u32)h * 23;
}

NOINLINE u32 NAME(calls)(u32 a, u32 b) {
  u32 r = NAME(leaf)(a, b) + OTHER(leaf)(b, a);
  r ^= NAME(many)(a, b, a ^ b, a + b, a - b, a * b, ~a, ((u64)a << 32) | b);
  r += NAME(fib)(a & 15, 0) * OTHER(fib)(b & 7, 0);
  return r;
}

static u32 NAME(op0)(u32 a, u32 b) { return a + b; }
static u32 NAME(op1)(u32 a, u32 b) { return a - b; }
static u32 NAME(op2)(u32 a, u32 b) { return a * b; }
static u32 NAME(op3)(u32 a, u32 b) { return a ^ rotl(b, 5); }
static const fn2 NAME(ops)[6] = { NAME(op0), NAME(op1), NAME(op2), NAME(op3), OTHER(leaf), NAME(leaf) };

NOINLINE u32 NAME(indirect)(u32 a, u32 b) {       // calls through function pointers, some to the other state
  u32 r = 0;
  for (u32 i = 0; i < 12; i++) r = NAME(ops)[(a >> (i * 2)) % 6](r + a, b + i);
  return r;
}

NOINLINE u32 NAME(sw)(u32 a, u32 b) {             // dense switch: compilers emit a jump table
  u32 r = b;
  for (u32 i = 0; i < 8; i++) {
    switch ((a >> (i * 4)) & 15) {
      case 0: r += 1; break; case 1: r ^= 0x55; break; case 2: r = rotl(r, 3); break; case 3: r -= b; break;
      case 4: r *= 3; break; case 5: r |= 0x100; break; case 6: r &= ~0x10u; break; case 7: r += a; break;
      case 8: r ^= a >> 3; break; case 9: r = ~r; break; case 10: r += i; break; case 11: r -= i * 7; break;
      case 12: r ^= r << 5; break; case 13: r += 0xABCDEF; break; case 14: r = rotl(r, 17); break; default: r ^= b << 1; break;
    }
  }
  return r;
}

NOINLINE u32 NAME(div)(u32 a, u32 b) {            // divisions become calls to the run-time helpers
  u32 d = (b & 0xFFFF) | 1; s32 sd = (s32)((b >> 8) | 1);
  return a / d + a % d * 3 + (u32)((s32)a / sd) * 5 + (u32)((s32)a % sd) * 7;
}

NOINLINE u32 NAME(wide)(u32 a, u32 b) {           // 64-bit arithmetic
  u64 x = ((u64)a << 32) | b, y = ((u64)b << 32) | (a ^ 0xFFFF);
  u64 r = x + y; r ^= x - y; r += x * y; r ^= x >> 7; r += y << 9; r ^= (u64)((s64)x >> 13);
  r += (x < y) + ((s64)x < (s64)y) * 2 + (x == y) * 4;
  r ^= x >> (a & 63); r += y << (b & 63);
  return (u32)r ^ (u32)(r >> 32);
}
