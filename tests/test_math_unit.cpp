#include <cstdint>

#include "nds/arm9_io.h"
#include "nds/math_unit.h"
#include "test.h"

namespace {

struct Fixture {
  bd::nds::Arm9Io io;
  bd::nds::MathUnit math;
  Fixture() { math.attach(io); }
  void write64(std::uint32_t address, std::uint64_t value) {
    io.write(address, 4, static_cast<std::uint32_t>(value));
    io.write(address + 4, 4, static_cast<std::uint32_t>(value >> 32));
  }
  std::uint64_t read64(std::uint32_t address) {
    return io.read(address, 4) | (static_cast<std::uint64_t>(io.read(address + 4, 4)) << 32);
  }
  void divide(unsigned mode, std::uint64_t numerator, std::uint64_t denominator) {
    io.write(0x04000280, 2, mode);
    write64(0x04000290, numerator);
    write64(0x04000298, denominator);
  }
  std::uint64_t quotient() { return read64(0x040002A0); }
  std::uint64_t remainder() { return read64(0x040002A8); }
  std::uint32_t root(unsigned mode, std::uint64_t value) {
    io.write(0x040002B0, 2, mode);
    write64(0x040002B8, value);
    return io.read(0x040002B4, 4);
  }
};

std::uint64_t s(std::int64_t v) { return static_cast<std::uint64_t>(v); }

}  // namespace

TEST(math_unit_divides_32_by_32) {
  Fixture f;
  f.divide(0, 100, 7);
  CHECK(f.quotient() == 14 && f.remainder() == 2);
  f.divide(0, s(-100) & 0xFFFFFFFF, 7);  // Only the low halves count; results are sign-extended.
  CHECK(f.quotient() == s(-14) && f.remainder() == s(-2));
  f.divide(0, 100, s(-7) & 0xFFFFFFFF);
  CHECK(f.quotient() == s(-14) && f.remainder() == 2);
  f.divide(0, 0xDEADBEEF00000064ull, 0x1234567800000007ull);  // The upper halves are ignored.
  CHECK(f.quotient() == 14 && f.remainder() == 2);
  CHECK((f.io.read(0x04000280, 2) & 0xC000) == 0);  // No error, never busy.
  CHECK(f.io.unmodelled().empty());
}

TEST(math_unit_divides_64_bit_numbers) {
  Fixture f;
  f.divide(1, 0x0000001000000000ull, 0x10);  // 64 by 32.
  CHECK(f.quotient() == 0x0000000100000000ull && f.remainder() == 0);
  f.divide(1, s(-1000000000000), 0xFFFFFFFF00000003ull);  // The denominator's upper half is ignored.
  CHECK(f.quotient() == s(-333333333333) && f.remainder() == s(-1));
  f.divide(1, 1000, s(-3) & 0xFFFFFFFF);  // The 32-bit denominator is signed.
  CHECK(f.quotient() == s(-333) && f.remainder() == 1);
  f.divide(2, 0x7FFFFFFFFFFFFFFFull, 0x0000000100000000ull);  // 64 by 64.
  CHECK(f.quotient() == 0x7FFFFFFF && f.remainder() == 0xFFFFFFFF);
  f.divide(3, 1000, 7);  // Mode 3 is the same as mode 1.
  CHECK(f.quotient() == 142 && f.remainder() == 6);
}

TEST(math_unit_division_by_zero) {
  Fixture f;
  f.divide(0, 5, 0);
  CHECK(f.quotient() == 0x00000000FFFFFFFFull);  // -1, with the upper half inverted.
  CHECK(f.remainder() == 5);
  CHECK((f.io.read(0x04000280, 2) & 0x4000) != 0);
  f.divide(0, s(-5) & 0xFFFFFFFF, 0);
  CHECK(f.quotient() == 0xFFFFFFFF00000001ull);  // +1, with the upper half inverted.
  CHECK(f.remainder() == s(-5));
  f.divide(2, 5, 0);
  CHECK(f.quotient() == s(-1) && f.remainder() == 5);
  f.divide(2, s(-5), 0);
  CHECK(f.quotient() == 1 && f.remainder() == s(-5));
  // The error flag looks at the whole denominator, whatever the mode: with
  // only the upper half set, a 32-bit division divides by zero unflagged.
  f.divide(0, 5, 0x0000000100000000ull);
  CHECK(f.quotient() == 0x00000000FFFFFFFFull);
  CHECK((f.io.read(0x04000280, 2) & 0x4000) == 0);
  f.divide(0, 5, 1);
  CHECK(f.quotient() == 5 && (f.io.read(0x04000280, 2) & 0x4000) == 0);
}

TEST(math_unit_division_overflow) {
  Fixture f;
  f.divide(0, 0x80000000, 0xFFFFFFFF);
  CHECK(f.quotient() == 0x0000000080000000ull && f.remainder() == 0);
  f.divide(1, 0x8000000000000000ull, 0xFFFFFFFF);
  CHECK(f.quotient() == 0x8000000000000000ull && f.remainder() == 0);
  f.divide(2, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull);
  CHECK(f.quotient() == 0x8000000000000000ull && f.remainder() == 0);
}

TEST(math_unit_results_follow_every_operand_write) {
  Fixture f;
  f.divide(0, 100, 7);
  f.io.write(0x04000298, 4, 5);  // Just the denominator.
  CHECK(f.quotient() == 20 && f.remainder() == 0);
  f.io.write(0x04000290, 1, 99);  // One byte of the numerator.
  CHECK(f.quotient() == 19 && f.remainder() == 4);
  f.io.write(0x04000280, 2, 2);  // Just the mode.
  CHECK(f.quotient() == 19);
  f.io.write(0x040002A0, 4, 12345);  // Results cannot be written.
  CHECK(f.quotient() == 19);
  CHECK(f.read64(0x04000290) == 99 && f.read64(0x04000298) == 5);
}

TEST(math_unit_square_root) {
  Fixture f;
  CHECK(f.root(0, 0) == 0);
  CHECK(f.root(0, 1) == 1);
  CHECK(f.root(0, 15) == 3);
  CHECK(f.root(0, 16) == 4);
  CHECK(f.root(0, 0xFFFFFFFF) == 0xFFFF);
  CHECK(f.root(0, 0x0000000100000010ull) == 4);  // 32-bit mode: the upper half is ignored.
  CHECK(f.root(1, 0x0000000100000000ull) == 0x10000);
  CHECK(f.root(1, 0xFFFFFFFFFFFFFFFFull) == 0xFFFFFFFF);
  CHECK(f.root(1, 0xFFFFFFFE00000001ull) == 0xFFFFFFFF);  // 0xFFFFFFFF squared, exactly.
  CHECK(f.root(1, 0xFFFFFFFE00000000ull) == 0xFFFFFFFE);
  CHECK((f.io.read(0x040002B0, 2) & 0x8000) == 0);  // Never busy.
  f.io.write(0x040002B0, 2, 0);  // Changing the mode recomputes.
  CHECK(f.io.read(0x040002B4, 4) == 0);
}
