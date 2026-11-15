// The console's buttons and its lid.
//
// Ten buttons are read by the main program from a register of its own. The
// other two, X and Y, and the lid's sensor are wired to the second processor,
// which passes them on (second_processor.h). In both places a bit is 0 while
// its button is held.
//
// The register that can raise an interrupt on a combination of buttons
// (0x04000132) is not modelled.
#pragma once

#include <cstdint>
#include <functional>

#include "nds/arm9_io.h"

namespace bd::nds {

// In the order of the bits in set_held.
enum class Button { A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y };

class Keypad {
 public:
  static constexpr std::uint32_t kAllButtons = 0x0FFF;

  // The main program's button register, 0x04000130.
  void attach(Arm9Io& io);

  // Which buttons are held: bit n for the nth Button.
  void set_held(std::uint32_t buttons);
  std::uint32_t held() const { return held_; }
  void set_lid_closed(bool closed);
  bool lid_closed() const { return lid_closed_; }

  // What the main program reads: A to L in bits 0-9, 0 = held.
  std::uint16_t main_register() const { return static_cast<std::uint16_t>(~held_ & 0x03FFu); }
  // What the second processor reads from its own register (0x04000136): X in
  // bit 0 and Y in bit 1, 0 = held; bit 3, a button only development
  // consoles have, never held; bit 6, 0 while the touch screen is touched;
  // bit 7, 1 while the lid is closed. The remaining low bits read as 1.
  std::uint16_t second_processor_register(bool screen_touched) const;

  // Called after the buttons or the lid changed.
  std::function<void()> on_change;

 private:
  std::uint32_t held_ = 0;
  bool lid_closed_ = false;
};

}  // namespace bd::nds
