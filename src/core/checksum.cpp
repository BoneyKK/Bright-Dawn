#include "core/checksum.h"

#include <array>

namespace bd {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

}  // namespace

std::uint16_t crc16(ByteSpan data) {
  std::uint32_t crc = 0xFFFF;
  for (std::uint8_t byte : data) {
    crc ^= byte;
    for (int k = 0; k < 8; ++k) crc = (crc & 1) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1);
  }
  return static_cast<std::uint16_t>(crc);
}

std::uint32_t crc32(ByteSpan data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uint8_t byte : data) crc = kCrc32Table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace bd
