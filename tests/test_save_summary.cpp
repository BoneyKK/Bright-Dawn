// What the launcher reads of a save: made-up blocks laid out as the game's.
#include <cstring>
#include <vector>

#include "test.h"
#include "ui/save_summary.h"

using bd::ui::play_time;
using bd::ui::read_save_summary;

namespace {

void put(std::vector<std::uint8_t>& s, std::size_t at, std::uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) s[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// A block at `at`, with what the launcher reads.
void block(std::vector<std::uint8_t>& s, std::size_t at, std::uint32_t count, const char16_t* name, int level,
           std::uint32_t frames, std::uint32_t coins) {
  std::memcpy(&s[at], "TLMCDDSG", 8);
  put(s, at + 0x14, count, 4);
  put(s, at + 0x38, frames, 4);
  for (std::size_t i = 0; name[i] != 0; ++i) put(s, at + 0x48 + 2 * i, name[i], 2);
  put(s, at + 0x70, static_cast<std::uint32_t>(level), 2);
  put(s, at + 0x2C08, coins, 4);
}

}  // namespace

TEST(save_summary_takes_the_newest_block) {
  std::vector<std::uint8_t> save(0x40000, 0xFF);
  block(save, 0x0000, 7, u"Old", 3, 1000, 5);
  block(save, 0x4000, 7, u"Old", 3, 1000, 5);
  block(save, 0x8000, 9, u"Moré", 22, 2502785, 27662);
  block(save, 0x30000, 8, u"Older", 20, 900, 1);
  const auto s = read_save_summary(save);
  CHECK(s.has_value());
  CHECK(s->name == "Mor\xC3\xA9");
  CHECK(s->level == 22);
  CHECK(s->frames_played == 2502785u);
  CHECK(s->coins == 27662u);
  CHECK(play_time(s->frames_played) == "11:35");
}

TEST(save_summary_finds_nothing_in_an_empty_or_broken_save) {
  CHECK(!read_save_summary(std::vector<std::uint8_t>(0x40000, 0xFF)).has_value());
  CHECK(!read_save_summary(std::vector<std::uint8_t>(0x40000, 0x00)).has_value());
  CHECK(!read_save_summary(std::vector<std::uint8_t>(100, 0)).has_value());
  std::vector<std::uint8_t> save(0x40000, 0);
  block(save, 0, 1, u"Half", 0, 10, 0);  // No level: not finished.
  CHECK(!read_save_summary(save).has_value());
}

TEST(play_time_is_hours_and_minutes) {
  CHECK(play_time(0) == "0:00");
  CHECK(play_time(60 * 60 * 61) == "1:01");
}
