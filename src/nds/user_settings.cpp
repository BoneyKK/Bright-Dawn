#include "nds/user_settings.h"

#include "core/checksum.h"

namespace bd::nds {
namespace {

void put16(std::array<std::uint8_t, kUserSettingsBlockSize>& block, std::size_t at, std::uint16_t value) {
  block[at] = static_cast<std::uint8_t>(value);
  block[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

}  // namespace

Result<std::array<std::uint8_t, kUserSettingsBlockSize>> user_settings_block(const UserSettings& settings) {
  if (static_cast<unsigned>(settings.language) > 5)
    return Error{ErrorCode::InvalidArgument, "the console language is not one the firmware knows"};
  if (settings.favourite_colour > 15)
    return Error{ErrorCode::InvalidArgument, "the favourite colour must be between 0 and 15"};
  if (settings.birthday_month < 1 || settings.birthday_month > 12)
    return Error{ErrorCode::InvalidArgument, "the birthday month must be between 1 and 12"};
  if (settings.birthday_day < 1 || settings.birthday_day > 31)
    return Error{ErrorCode::InvalidArgument, "the birthday day must be between 1 and 31"};
  if (settings.nickname.size() > UserSettings::kLongestNickname)
    return Error{ErrorCode::InvalidArgument, "the nickname is longer than 10 characters"};
  if (settings.message.size() > UserSettings::kLongestMessage)
    return Error{ErrorCode::InvalidArgument, "the message is longer than 26 characters"};

  std::array<std::uint8_t, kUserSettingsBlockSize> block{};
  put16(block, 0x00, 5);  // Version of the layout.
  block[0x02] = settings.favourite_colour;
  block[0x03] = settings.birthday_month;
  block[0x04] = settings.birthday_day;
  for (std::size_t i = 0; i < settings.nickname.size(); ++i)
    put16(block, 0x06 + 2 * i, static_cast<std::uint16_t>(settings.nickname[i]));
  put16(block, 0x1A, static_cast<std::uint16_t>(settings.nickname.size()));
  for (std::size_t i = 0; i < settings.message.size(); ++i)
    put16(block, 0x1C + 2 * i, static_cast<std::uint16_t>(settings.message[i]));
  put16(block, 0x50, static_cast<std::uint16_t>(settings.message.size()));

  // Touch screen calibration: two points, each as the raw reading and the
  // pixel it stands for. Raw readings are taken to be the pixel times 16,
  // the mapping emulators without a firmware image use (NooDS). To be looked
  // at again when the touch screen is modelled.
  put16(block, 0x58, 0x0000);  // First point, raw.
  put16(block, 0x5A, 0x0000);
  block[0x5C] = 0;             // First point, pixel.
  block[0x5D] = 0;
  put16(block, 0x5E, 255 << 4);  // Second point, raw.
  put16(block, 0x60, 191 << 4);
  block[0x62] = 255;             // Second point, pixel.
  block[0x63] = 191;

  put16(block, 0x64, static_cast<std::uint16_t>(settings.language));  // Language, in the low three bits.
  put16(block, 0x70, 0);  // Update count.
  put16(block, 0x72, crc16(ByteSpan(block.data(), 0x70)));
  return block;
}

std::uint16_t chosen_wireless_channels(std::uint16_t firmware_channels) {
  // Channel 1 in bit 0, and no more than 13 channels.
  const unsigned allowed = (firmware_channels >> 1) & 0x1FFFu;
  if (allowed == 0) return 0;
  int lowest = 0;
  while ((allowed & (1u << lowest)) == 0) ++lowest;
  int highest = 15;
  while (highest != 0 && (allowed & (1u << highest)) == 0) --highest;
  const int span = highest - lowest;
  if (span < 5) return static_cast<std::uint16_t>(1u << lowest);

  // The allowed channel nearest the middle, looking above it first.
  int middle = (highest + lowest) / 2;
  for (int i = 0; i < span; ++i) {
    middle += (i % 2 != 0) ? i : -i;
    if ((allowed & (1u << middle)) != 0) break;
  }
  unsigned chosen = (1u << lowest) | (1u << highest);
  if (highest - middle >= 5 && middle - lowest >= 5) chosen |= 1u << middle;
  return static_cast<std::uint16_t>(chosen);
}

}  // namespace bd::nds
