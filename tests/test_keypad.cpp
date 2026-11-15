// The buttons and the lid: the main program's register, and what the second
// processor passes on.
#include <cstdint>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/keypad.h"
#include "nds/save_memory.h"
#include "nds/second_processor.h"
#include "test.h"

namespace {

using bd::nds::Button;
using bd::nds::Keypad;

constexpr std::uint32_t bit(Button button) { return 1u << static_cast<unsigned>(button); }

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

}  // namespace

TEST(keypad_register_reads_all_ones_with_nothing_held) {
  bd::nds::Arm9Io io;
  Keypad keypad;
  keypad.attach(io);
  CHECK(io.read(0x04000130, 2) == 0x03FF);
  CHECK(io.read(0x04000130, 1) == 0xFF && io.read(0x04000131, 1) == 0x03);
  CHECK(io.unmodelled().empty());
}

TEST(keypad_register_has_a_zero_bit_for_each_button_held) {
  bd::nds::Arm9Io io;
  Keypad keypad;
  keypad.attach(io);
  const Button order[] = {Button::A, Button::B, Button::Select, Button::Start, Button::Right,
                          Button::Left, Button::Up, Button::Down, Button::R, Button::L};
  for (unsigned i = 0; i < 10; ++i) {
    keypad.set_held(bit(order[i]));
    CHECK(io.read(0x04000130, 2) == (0x03FFu & ~(1u << i)));
  }
  keypad.set_held(bit(Button::A) | bit(Button::Start) | bit(Button::L));
  CHECK(io.read(0x04000130, 2) == 0x01F6);
  // X and Y are not in this register, and neither is the lid.
  keypad.set_held(bit(Button::X) | bit(Button::Y));
  keypad.set_lid_closed(true);
  CHECK(io.read(0x04000130, 2) == 0x03FF);
  keypad.set_held(0);
  CHECK(io.read(0x04000130, 2) == 0x03FF);
}

TEST(keypad_register_cannot_be_written) {
  bd::nds::Arm9Io io;
  Keypad keypad;
  keypad.attach(io);
  io.write(0x04000130, 2, 0x0000);
  CHECK(io.read(0x04000130, 2) == 0x03FF);
  CHECK(io.unmodelled().empty());
}

TEST(keypad_second_processor_register) {
  Keypad keypad;
  CHECK(keypad.second_processor_register(false) == 0x007F);
  CHECK(keypad.second_processor_register(true) == 0x003F);  // Touched: bit 6 clear.
  keypad.set_held(bit(Button::X));
  CHECK(keypad.second_processor_register(false) == 0x007E);
  keypad.set_held(bit(Button::Y));
  CHECK(keypad.second_processor_register(false) == 0x007D);
  keypad.set_held(Keypad::kAllButtons);
  CHECK(keypad.second_processor_register(false) == 0x007C);  // Only X and Y are here.
  keypad.set_held(0);
  keypad.set_lid_closed(true);
  CHECK(keypad.second_processor_register(false) == 0x00FF);
}

TEST(keypad_reports_changes_once) {
  Keypad keypad;
  int changes = 0;
  keypad.on_change = [&] { ++changes; };
  keypad.set_held(0);                // No change.
  keypad.set_lid_closed(false);
  CHECK(changes == 0);
  keypad.set_held(bit(Button::A));
  keypad.set_held(bit(Button::A));   // The same again.
  CHECK(changes == 1);
  keypad.set_held(bit(Button::A) | 0xF000);  // Bits that are no button are dropped.
  CHECK(changes == 1 && keypad.held() == bit(Button::A));
  keypad.set_lid_closed(true);
  keypad.set_lid_closed(true);
  CHECK(changes == 2);
}

TEST(second_processor_passes_on_its_buttons) {
  NoIo no_io;
  bd::nds::Arm9Bus bus(no_io);
  bd::nds::SaveMemory save;
  bd::nds::SecondProcessor second(bus, 0x00001FC2, save);
  Keypad keypad;
  bus.write32(0x027FFFA4, 0xEEEEEEEE);
  bus.write32(0x027FFFA8, 0xEEEEEEEE);
  second.report_buttons(keypad.second_processor_register(false));
  CHECK(bus.read16(0x027FFFA8) == 0x2C00);  // Nothing held, lid open.
  CHECK(bus.read16(0x027FFFAA) == 0xEEEE && bus.read16(0x027FFFA6) == 0xEEEE);  // Only that word.
  keypad.set_held(bit(Button::X));
  second.report_buttons(keypad.second_processor_register(false));
  CHECK(bus.read16(0x027FFFA8) == 0x2800);
  keypad.set_held(bit(Button::Y) | bit(Button::A));
  second.report_buttons(keypad.second_processor_register(false));
  CHECK(bus.read16(0x027FFFA8) == 0x2400);
  keypad.set_held(0);
  keypad.set_lid_closed(true);
  second.report_buttons(keypad.second_processor_register(false));
  CHECK(bus.read16(0x027FFFA8) == 0xAC00);
  // Whether the screen is touched is not part of the word.
  keypad.set_lid_closed(false);
  second.report_buttons(keypad.second_processor_register(true));
  CHECK(bus.read16(0x027FFFA8) == 0x2C00);
  CHECK(bus.read16(0x02FFFFA8) == 0x2C00);  // Where the game reads it.
}
