// Run-time helpers that the compiler calls in the ARM build of the guest
// corpus (division, 64-bit shifts, bit counting, memory fills). They are
// written without the operations they implement and built with -fno-builtin,
// so the compiler cannot turn them back into calls to themselves. Not part of
// the host build.
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
static u64 udivmod(u32 n, u32 d) {
  u32 q = 0, r = 0;
  for (int i = 31; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1u << i; } }
  return ((u64)r << 32) | q;
}
u32 __aeabi_uidiv(u32 n, u32 d) { return (u32)udivmod(n, d); }
u64 __aeabi_uidivmod(u32 n, u32 d) { return udivmod(n, d); }          // quotient in r0, remainder in r1
static u64 sdivmod(s32 n, s32 d) {
  u32 un = n < 0 ? 0u - (u32)n : (u32)n, ud = d < 0 ? 0u - (u32)d : (u32)d;
  u64 v = udivmod(un, ud); u32 q = (u32)v, r = (u32)(v >> 32);
  if ((n < 0) != (d < 0)) q = 0u - q;
  if (n < 0) r = 0u - r;
  return ((u64)r << 32) | q;
}
s32 __aeabi_idiv(s32 n, s32 d) { return (s32)(u32)sdivmod(n, d); }
u64 __aeabi_idivmod(s32 n, s32 d) { return sdivmod(n, d); }
u64 __aeabi_llsl(u64 v, int n) { u32 lo = (u32)v, hi = (u32)(v >> 32); if (n == 0) return v; if (n >= 32) return (u64)(lo << (n - 32)) << 32; return ((u64)((hi << n) | (lo >> (32 - n))) << 32) | (lo << n); }
u64 __aeabi_llsr(u64 v, int n) { u32 lo = (u32)v, hi = (u32)(v >> 32); if (n == 0) return v; if (n >= 32) return hi >> (n - 32); return ((u64)(hi >> n) << 32) | ((lo >> n) | (hi << (32 - n))); }
u64 __aeabi_lasr(u64 v, int n) { u32 lo = (u32)v; s32 hi = (s32)(v >> 32); if (n == 0) return v; if (n >= 32) return ((u64)(u32)(hi >> 31) << 32) | (u32)(hi >> (n - 32)); return ((u64)(u32)(hi >> n) << 32) | ((lo >> n) | ((u32)hi << (32 - n))); }
u64 __aeabi_lmul(u64 a, u64 b) {
  u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
  u64 lo = (u64)al * bl; return lo + ((u64)(al * bh + ah * bl) << 32);
}
int __clzsi2(u32 v) { int n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
int __ctzsi2(u32 v) { int n = 0; if (!v) return 32; while (!(v & 1)) { v >>= 1; n++; } return n; }
int __popcountsi2(u32 v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
void *memset(void *p, int c, unsigned n) { unsigned char *q = p; while (n--) *q++ = (unsigned char)c; return p; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *q = d; const unsigned char *r = s; while (n--) *q++ = *r++; return d; }
void __aeabi_memclr(void *p, unsigned n) { memset(p, 0, n); }
void __aeabi_memclr4(void *p, unsigned n) { memset(p, 0, n); }
void __aeabi_memclr8(void *p, unsigned n) { memset(p, 0, n); }
void __aeabi_memcpy(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memset(void *p, unsigned n, int c) { memset(p, c, n); }
void __aeabi_memset4(void *p, unsigned n, int c) { memset(p, c, n); }
void __aeabi_memset8(void *p, unsigned n, int c) { memset(p, c, n); }
