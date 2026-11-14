// The DS's hardware divider and square root: registers a program writes
// operands to and reads results from.
//
// On the console a result takes a few dozen cycles and a "busy" bit says so.
// Here results are there at once and the bit is never set.
#pragma once

#include <cstdint>

#include "nds/arm9_io.h"

namespace bd::nds {

class MathUnit {
 public:
  // Registers: division control (0x04000280), numerator (0x04000290),
  // denominator (0x04000298), quotient (0x040002A0), remainder (0x040002A8);
  // square root control (0x040002B0), result (0x040002B4), operand
  // (0x040002B8). The 64-bit ones are two 32-bit registers each.
  void attach(Arm9Io& io);

  // The largest r with r * r <= value.
  static std::uint32_t square_root(std::uint64_t value);

 private:
  void divide();
  void root();

  std::uint32_t divide_control_ = 0, root_control_ = 0;
  std::uint64_t numerator_ = 0, denominator_ = 0, quotient_ = 0, remainder_ = 0;
  std::uint64_t operand_ = 0;
  std::uint32_t root_ = 0;
};

}  // namespace bd::nds
