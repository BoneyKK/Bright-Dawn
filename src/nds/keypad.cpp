#include "nds/keypad.h"

namespace bd::nds {

void Keypad::attach(Arm9Io& io) {
  // Read only: writes go nowhere.
  io.add(0x04000130, 2, [this] { return static_cast<std::uint32_t>(main_register()); }, nullptr);
}

void Keypad::set_held(std::uint32_t buttons) {
  buttons &= kAllButtons;
  if (buttons == held_) return;
  held_ = buttons;
  if (on_change) on_change();
}

void Keypad::set_lid_closed(bool closed) {
  if (closed == lid_closed_) return;
  lid_closed_ = closed;
  if (on_change) on_change();
}

std::uint16_t Keypad::second_processor_register(bool screen_touched) const {
  std::uint32_t value = 0x007F;
  if (held_ & (1u << static_cast<unsigned>(Button::X))) value &= ~0x01u;
  if (held_ & (1u << static_cast<unsigned>(Button::Y))) value &= ~0x02u;
  if (screen_touched) value &= ~0x40u;
  if (lid_closed_) value |= 0x80u;
  return static_cast<std::uint16_t>(value);
}

}  // namespace bd::nds
