#include "nds/math_unit.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u32 kDivideMode = 3;            // 0: 32 by 32 bits. 1, 3: 64 by 32. 2: 64 by 64.
constexpr u32 kDivideByZero = 1u << 14;   // Set while the whole 64-bit denominator is zero.
constexpr u32 kRootMode = 1;              // 0: 32-bit operand. 1: 64-bit.

constexpr s64 kMin64 = static_cast<s64>(u64{1} << 63);
constexpr s32 kMin32 = static_cast<s32>(u32{1} << 31);

// Reads and writes of one half of a 64-bit register.
u32 half(u64 value, int high) { return static_cast<u32>(value >> (high ? 32 : 0)); }
void set_half(u64& reg, int high, u32 value, u32 mask) {
  const unsigned shift = high ? 32 : 0;
  reg = (reg & ~(static_cast<u64>(mask) << shift)) | (static_cast<u64>(value & mask) << shift);
}

}  // namespace

void MathUnit::attach(Arm9Io& io) {
  io.add(0x04000280, 4, [this] { return divide_control_; },
         [this](u32 v, u32 m) {
           m &= kDivideMode;
           divide_control_ = (divide_control_ & ~m) | (v & m);
           divide();
         });
  for (int high = 0; high < 2; ++high) {
    const u32 offset = 4 * static_cast<u32>(high);
    io.add(0x04000290 + offset, 4, [this, high] { return half(numerator_, high); },
           [this, high](u32 v, u32 m) {
             set_half(numerator_, high, v, m);
             divide();
           });
    io.add(0x04000298 + offset, 4, [this, high] { return half(denominator_, high); },
           [this, high](u32 v, u32 m) {
             set_half(denominator_, high, v, m);
             divide();
           });
    io.add(0x040002A0 + offset, 4, [this, high] { return half(quotient_, high); }, nullptr);
    io.add(0x040002A8 + offset, 4, [this, high] { return half(remainder_, high); }, nullptr);
    io.add(0x040002B8 + offset, 4, [this, high] { return half(operand_, high); },
           [this, high](u32 v, u32 m) {
             set_half(operand_, high, v, m);
             root();
           });
  }
  io.add(0x040002B0, 4, [this] { return root_control_; },
         [this](u32 v, u32 m) {
           m &= kRootMode;
           root_control_ = (root_control_ & ~m) | (v & m);
           root();
         });
  io.add(0x040002B4, 4, [this] { return root_; }, nullptr);
}

// Signed division. The three cases that have no mathematical answer give
// what the hardware gives (as NooDS has them, which follows GBATEK):
// division by zero returns +1 or -1 against the numerator's sign and the
// numerator as remainder; the most negative number divided by -1 returns
// itself. In the 32-bit mode the results are sign-extended to 64 bits,
// except that in those two cases the upper half of the quotient comes out
// inverted.
void MathUnit::divide() {
  if (denominator_ == 0) {
    divide_control_ |= kDivideByZero;
  } else {
    divide_control_ &= ~kDivideByZero;
  }
  const u32 mode = divide_control_ & kDivideMode;
  if (mode == 0) {
    const s32 n = static_cast<s32>(numerator_), d = static_cast<s32>(denominator_);
    if (d == 0) {
      quotient_ = static_cast<u64>(static_cast<s64>(n < 0 ? 1 : -1)) ^ 0xFFFFFFFF00000000ull;
      remainder_ = static_cast<u64>(static_cast<s64>(n));
    } else if (n == kMin32 && d == -1) {
      quotient_ = static_cast<u64>(static_cast<s64>(n)) ^ 0xFFFFFFFF00000000ull;
      remainder_ = 0;
    } else {
      quotient_ = static_cast<u64>(static_cast<s64>(n / d));
      remainder_ = static_cast<u64>(static_cast<s64>(n % d));
    }
    return;
  }
  const s64 n = static_cast<s64>(numerator_);
  const s64 d = mode == 2 ? static_cast<s64>(denominator_) : static_cast<s64>(static_cast<s32>(denominator_));
  if (d == 0) {
    quotient_ = static_cast<u64>(static_cast<s64>(n < 0 ? 1 : -1));
    remainder_ = static_cast<u64>(n);
  } else if (n == kMin64 && d == -1) {
    quotient_ = static_cast<u64>(n);
    remainder_ = 0;
  } else {
    quotient_ = static_cast<u64>(n / d);
    remainder_ = static_cast<u64>(n % d);
  }
}

u32 MathUnit::square_root(u64 value) {
  u64 root = 0;
  for (u64 bit = u64{1} << 31; bit != 0; bit >>= 1) {
    const u64 trial = root | bit;
    if (trial * trial <= value) root = trial;
  }
  return static_cast<u32>(root);
}

void MathUnit::root() { root_ = square_root((root_control_ & kRootMode) ? operand_ : (operand_ & 0xFFFFFFFFull)); }

}  // namespace bd::nds
