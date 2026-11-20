// What the launcher shows of the player's save: whose game it is, how far,
// and how long it has been played, read from the save file the program keeps
// (nds/save_keeper.h) as the game wrote it.
//
// The game keeps its save in blocks of 16 KB, each written twice; every
// time it saves, it writes the next pair and counts on. The newest block is
// the one with the highest count. Each block begins with:
//   0x00  "TLMCDDSG"
//   0x14  how many times the game has been saved (32 bits)
//   0x38  the time played, in frames (60 a second; 32 bits)
//   0x48  the leader's name, up to 20 characters of UTF-16, ended by 0
//   0x70  the leader's level (16 bits)
// and 0x2C08 holds the coins (32 bits). Found by comparing a save with what
// the game's own list of saves shows for it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bd::ui {

struct SaveSummary {
  std::string name;  // UTF-8.
  int level = 0;
  std::uint32_t frames_played = 0;
  std::uint32_t coins = 0;
};

// The newest save in `save`, or nothing when it holds none.
std::optional<SaveSummary> read_save_summary(std::span<const std::uint8_t> save);

// Time played as the game shows it: hours and minutes ("11:35").
std::string play_time(std::uint32_t frames);

}  // namespace bd::ui
