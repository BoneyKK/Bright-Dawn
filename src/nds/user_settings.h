// What the console knows about its owner, and what it knows about itself.
//
// A DS keeps its owner's settings (language, name, birthday and so on) and
// its wireless network address in its firmware. Before a game's main program
// looks at them, the game's second-processor program has read them out of the
// firmware and left them at fixed places near the top of main memory. This
// project has no firmware image, so the settings come from here instead.
//
// What is written where was read out of the second-processor program of
// Golden Sun: Dark Dawn (its start-up routine) and of the main program's
// routines that fetch the owner's details and the network address. The layout
// of the settings block agrees with GBATEK's description of the firmware's
// "user settings".
//
// The game does look: one of its routines stops the game for good when the
// network address is all zeros, which no real console's is.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "core/result.h"

namespace bd::nds {

struct UserSettings {
  // The values the firmware uses.
  enum class Language : std::uint8_t {
    Japanese = 0,
    English = 1,
    French = 2,
    German = 3,
    Italian = 4,
    Spanish = 5,
  };

  static constexpr std::size_t kLongestNickname = 10;  // Characters.
  static constexpr std::size_t kLongestMessage = 26;

  Language language = Language::English;
  std::uint8_t favourite_colour = 0;  // 0 to 15.
  std::uint8_t birthday_month = 1;    // 1 to 12.
  std::uint8_t birthday_day = 1;      // 1 to 31.
  std::u16string nickname = u"Player";
  std::u16string message;

  // The console's wireless network address. Invented: it starts with a
  // prefix real consoles use, as the addresses emulators make up do, and it
  // is never sent anywhere, because this project has no wireless.
  std::array<std::uint8_t, 6> network_address = {0x00, 0x09, 0xBF, 0x12, 0x34, 0x56};

  // The wireless channels the console may use, as the firmware stores them:
  // bit n set = channel n allowed. Channels 1 to 13, as on consoles sold in
  // Europe.
  std::uint16_t wireless_channels = 0x3FFE;
};

// Where the second-processor program leaves these in main memory.
constexpr std::uint32_t kUserSettingsAddress = 0x027FFC80;     // The block below.
constexpr std::uint32_t kNetworkAddressAddress = 0x027FFCF4;   // Six bytes.
constexpr std::uint32_t kWirelessChannelsAddress = 0x027FFCFA; // 16 bits: see chosen_wireless_channels.

// The settings block as the firmware stores it: 0x70 bytes of settings, a
// 16-bit update count and the block's checksum. Fails if a value is outside
// what the firmware allows; nothing is cut short or corrected.
constexpr std::size_t kUserSettingsBlockSize = 0x74;
Result<std::array<std::uint8_t, kUserSettingsBlockSize>> user_settings_block(const UserSettings& settings);

// The channels the second-processor program picks out of the ones the
// firmware allows, for games to offer: the lowest and the highest, and one
// near the middle if all three are at least five channels apart; only the
// lowest if lowest and highest are less than five apart. `firmware_channels`
// is the firmware's value (bit n = channel n); in the result, bit n-1 stands
// for channel n.
std::uint16_t chosen_wireless_channels(std::uint16_t firmware_channels);

}  // namespace bd::nds
