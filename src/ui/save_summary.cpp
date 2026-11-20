#include "ui/save_summary.h"

#include <cstdio>
#include <cstring>

namespace bd::ui {
namespace {

constexpr std::size_t kBlock = 0x4000;
constexpr char kMagic[] = "TLMCDDSG";
constexpr std::size_t kCount = 0x14, kFrames = 0x38, kName = 0x48, kNameLength = 20, kLevel = 0x70, kCoins = 0x2C08;

std::uint32_t read(std::span<const std::uint8_t> s, std::size_t at, unsigned bytes) {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < bytes; ++i) v |= static_cast<std::uint32_t>(s[at + i]) << (8 * i);
  return v;
}

void put_utf8(std::string& out, std::uint32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

}  // namespace

std::optional<SaveSummary> read_save_summary(std::span<const std::uint8_t> save) {
  std::optional<SaveSummary> newest;
  std::uint32_t newest_count = 0;
  for (std::size_t at = 0; at + kBlock <= save.size(); at += kBlock) {
    const std::span<const std::uint8_t> block = save.subspan(at, kBlock);
    if (std::memcmp(block.data(), kMagic, 8) != 0) continue;
    const std::uint32_t count = read(block, kCount, 4);
    const int level = static_cast<int>(read(block, kLevel, 2));
    if (level < 1 || level > 99) continue;  // Not a block the game finished writing.
    if (newest && count <= newest_count) continue;
    SaveSummary s;
    s.level = level;
    s.frames_played = read(block, kFrames, 4);
    s.coins = read(block, kCoins, 4);
    for (std::size_t i = 0; i < kNameLength; ++i) {
      const std::uint32_t c = read(block, kName + 2 * i, 2);
      if (c == 0 || c == 0xFFFF) break;
      // Characters the game uses for its own purposes are not letters.
      put_utf8(s.name, (c < 0x20 || (c >= 0xD800 && c < 0xE000) || c >= 0xF000) ? '?' : c);
    }
    newest = std::move(s);
    newest_count = count;
  }
  return newest;
}

std::string play_time(std::uint32_t frames) {
  const std::uint32_t minutes = frames / 60 / 60;
  char text[32];
  std::snprintf(text, sizeof text, "%u:%02u", static_cast<unsigned>(minutes / 60), static_cast<unsigned>(minutes % 60));
  return text;
}

}  // namespace bd::ui
