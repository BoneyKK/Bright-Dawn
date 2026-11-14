// The console's settings, as the second-processor program leaves them in
// memory for the main program.
#include <array>
#include <cstdint>

#include "nds/arm9_bus.h"
#include "nds/second_processor.h"
#include "nds/user_settings.h"
#include "test.h"

namespace {

using bd::nds::UserSettings;

// The checksum the firmware uses, written out here so the test does not lean
// on the code it is checking: CRC-16, polynomial 0xA001, starting at 0xFFFF.
std::uint16_t reference_crc(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFF;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xA001u : 0u);
  }
  return static_cast<std::uint16_t>(crc);
}

unsigned word_at(const std::array<std::uint8_t, bd::nds::kUserSettingsBlockSize>& block, std::size_t at) {
  return block[at] | (block[at + 1] << 8);
}

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

}  // namespace

TEST(user_settings_block_has_the_firmware_layout) {
  UserSettings settings;
  settings.language = UserSettings::Language::German;
  settings.favourite_colour = 11;
  settings.birthday_month = 10;
  settings.birthday_day = 29;
  settings.nickname = u"Matthew";
  settings.message = u"Hi";
  const auto result = bd::nds::user_settings_block(settings);
  CHECK(result.ok());
  if (!result) return;
  const auto& block = result.value();
  CHECK(block.size() == 0x74);
  CHECK(word_at(block, 0x00) == 5);
  CHECK(block[0x02] == 11 && block[0x03] == 10 && block[0x04] == 29 && block[0x05] == 0);
  CHECK(word_at(block, 0x06) == 'M' && word_at(block, 0x08) == 'a' && word_at(block, 0x12) == 'w');
  CHECK(word_at(block, 0x14) == 0 && word_at(block, 0x18) == 0);  // Unused characters are zero.
  CHECK(word_at(block, 0x1A) == 7);
  CHECK(word_at(block, 0x1C) == 'H' && word_at(block, 0x1E) == 'i' && word_at(block, 0x20) == 0);
  CHECK(word_at(block, 0x50) == 2);
  CHECK((word_at(block, 0x64) & 7) == 3);
  // What the second-processor program requires of a block before it uses it.
  CHECK(word_at(block, 0x70) < 0x80);
  CHECK(word_at(block, 0x72) == reference_crc(block.data(), 0x70));
}

TEST(user_settings_block_takes_the_longest_names) {
  UserSettings settings;
  settings.nickname = u"0123456789";
  settings.message = u"abcdefghijklmnopqrstuvwxyz";
  const auto result = bd::nds::user_settings_block(settings);
  CHECK(result.ok());
  if (!result) return;
  const auto& block = result.value();
  CHECK(word_at(block, 0x18) == '9' && word_at(block, 0x1A) == 10);
  CHECK(word_at(block, 0x4E) == 'z' && word_at(block, 0x50) == 26);
  CHECK(word_at(block, 0x72) == reference_crc(block.data(), 0x70));
}

TEST(user_settings_block_keeps_characters_outside_ascii) {
  UserSettings settings;
  settings.nickname = u"Éloïse";
  const auto result = bd::nds::user_settings_block(settings);
  CHECK(result.ok());
  if (!result) return;
  CHECK(word_at(result.value(), 0x06) == 0x00C9 && word_at(result.value(), 0x0C) == 0x00EF);
}

TEST(user_settings_block_rejects_values_the_firmware_does_not_allow) {
  const auto rejected = [](void (*change)(UserSettings&)) {
    UserSettings settings;
    change(settings);
    return !bd::nds::user_settings_block(settings).ok();
  };
  CHECK(bd::nds::user_settings_block(UserSettings()).ok());  // The defaults are valid.
  CHECK(rejected([](UserSettings& s) { s.language = static_cast<UserSettings::Language>(6); }));
  CHECK(rejected([](UserSettings& s) { s.favourite_colour = 16; }));
  CHECK(rejected([](UserSettings& s) { s.birthday_month = 0; }));
  CHECK(rejected([](UserSettings& s) { s.birthday_month = 13; }));
  CHECK(rejected([](UserSettings& s) { s.birthday_day = 0; }));
  CHECK(rejected([](UserSettings& s) { s.birthday_day = 32; }));
  CHECK(rejected([](UserSettings& s) { s.nickname = u"01234567890"; }));
  CHECK(rejected([](UserSettings& s) { s.message = u"abcdefghijklmnopqrstuvwxyz!"; }));
}

TEST(wireless_channels_are_chosen_from_the_allowed_ones) {
  using bd::nds::chosen_wireless_channels;
  CHECK(chosen_wireless_channels(0x3FFE) == 0x1041);  // 1 to 13: channels 1, 7 and 13.
  CHECK(chosen_wireless_channels(0x0FFE) == 0x0421);  // 1 to 11: channels 1, 6 and 11.
  CHECK(chosen_wireless_channels(0x7FFE) == 0x1041);  // Channel 14 is never offered.
  CHECK(chosen_wireless_channels(0x4000) == 0);
  CHECK(chosen_wireless_channels(0x0000) == 0);
  CHECK(chosen_wireless_channels(0x0001) == 0);       // Bit 0 is not a channel.
  CHECK(chosen_wireless_channels(0x0040) == 0x0020);  // Channel 6 alone.
  CHECK(chosen_wireless_channels(0x001E) == 0x0001);  // 1 to 4: too close together, the lowest only.
  CHECK(chosen_wireless_channels(0x0042) == 0x0021);  // 1 and 6: both, nothing between them.
  CHECK(chosen_wireless_channels(0x2006) == 0x1001);  // 1, 2 and 13: 2 is too close to 1.
  CHECK(chosen_wireless_channels(0x2082) == 0x1041);  // 1, 7 and 13.
  CHECK(chosen_wireless_channels(0x2102) == 0x1081);  // 1, 8 and 13: five apart is enough.
  CHECK(chosen_wireless_channels(0x2202) == 0x1001);  // 1, 9 and 13: 9 is too close to 13.
}

TEST(second_processor_leaves_the_settings_in_memory) {
  NoIo no_io;
  bd::nds::Arm9Bus bus(no_io);
  bd::nds::SecondProcessor second(bus, 0x00001FC2);
  UserSettings settings;
  settings.language = UserSettings::Language::Spanish;
  settings.network_address = {0x00, 0x09, 0xBF, 0xA1, 0xB2, 0xC3};
  CHECK(second.start_up(settings).ok());

  const auto block = bd::nds::user_settings_block(settings);
  CHECK(block.ok());
  if (!block) return;
  bool same = true;
  for (std::uint32_t i = 0; i < 0x74; ++i) same = same && bus.read8(0x027FFC80 + i) == block.value()[i];
  CHECK(same);
  CHECK((bus.read16(0x027FFCE4) & 7) == 5);
  const std::uint8_t expected[6] = {0x00, 0x09, 0xBF, 0xA1, 0xB2, 0xC3};
  for (std::uint32_t i = 0; i < 6; ++i) CHECK(bus.read8(0x027FFCF4 + i) == expected[i]);
  CHECK(bus.read16(0x027FFCFA) == 0x1041);
  // Games read these through the copy of main memory higher up.
  CHECK(bus.read8(0x02FFFCF6) == 0xBF);
  CHECK(bus.read16(0x02FFFC80) == 5);
  // Their neighbours are untouched.
  CHECK(bus.read32(0x027FFC7C) == 0 && bus.read32(0x027FFCFC) == 0);
}

TEST(second_processor_writes_nothing_for_settings_that_are_not_valid) {
  NoIo no_io;
  bd::nds::Arm9Bus bus(no_io);
  bd::nds::SecondProcessor second(bus, 0x00001FC2);
  UserSettings settings;
  settings.birthday_month = 13;
  CHECK(!second.start_up(settings).ok());
  bool untouched = true;
  for (std::uint32_t i = 0; i < 0x80; ++i) untouched = untouched && bus.read8(0x027FFC80 + i) == 0;
  CHECK(untouched);
}

TEST(default_network_address_is_not_all_zeros) {
  // The game stops for good when it is.
  const UserSettings settings;
  unsigned any = 0;
  for (const std::uint8_t byte : settings.network_address) any |= byte;
  CHECK(any != 0);
}
