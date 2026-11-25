// Skips the story at the start of a new game of Golden Sun: Dark Dawn with
// Start: the story of Alchemy and the scenes after it, up to the game's
// title. From there the game goes on as usual, to the walk of Matthew and
// Isaac to their house.
//
// The game has a skip of its own: A or B ends the story, with its fade, when
// it is shown again from the title screen (movie 506), and goes to the
// title. The story of a new game is movie 503, which the game does not let
// be skipped. On Start this marks the story as movie 506 and presses A for
// the game for kPressFrames frames; the game then ends it as it ends the
// one from the title screen. At the title, the next press goes on to the
// new game itself.
//
// The story is a C++ object of the game's (in main memory): its virtual
// table at offset 0, the frames it has run at kFramesOffset and its movie at
// kMovieOffset. It is found by these when Start is pressed, and taken for
// the story only if its frame count goes up in the next kMostFindingFrames
// frames (it counts one frame in two of the console's).
#pragma once

#include <cstdint>
#include <vector>

namespace bd::game {

// The game's main memory, as the skip reads and writes it.
class GuestMemory {
 public:
  virtual ~GuestMemory() = default;
  virtual std::uint32_t read32(std::uint32_t address) const = 0;
  virtual void write32(std::uint32_t address, std::uint32_t value) = 0;
};

class OpeningSkip {
 public:
  // The buttons, as bits of the DS's button register.
  static constexpr std::uint32_t kA = 1u << 0, kB = 1u << 1;
  static constexpr std::uint32_t kStart = 1u << 3;

  static constexpr std::uint32_t kMainMemory = 0x02000000, kMainMemorySize = 0x00400000;
  static constexpr std::uint32_t kMovieOffset = 0x2a0, kFramesOffset = 0x26c;
  static constexpr std::uint32_t kNewGameMovie = 503, kSkippableMovie = 506;
  static constexpr std::uint32_t kMostFindingFrames = 4;
  // How long A is held for the game (it looks at the buttons each frame).
  static constexpr std::uint32_t kPressFrames = 2;

  enum class Phase {
    Idle,      // Watching for Start.
    Finding,   // Start was pressed: checking the objects found.
    Pressing,  // The story is marked as the skippable one: A is held.
  };

  // Once a frame, before the game's next one: `held` is what the player
  // holds; returns what the game is to be given. While skipping, that is
  // only the skip's own presses; the Start that began it, and the buttons
  // held when it ends, are not given until let go. `enabled`: Start
  // begins a skip (Settings, Quality of life).
  std::uint32_t frame(GuestMemory& memory, std::uint32_t held, bool enabled);

  // Whether the story was just found and marked (for the log).
  bool skipping() const { return phase_ == Phase::Pressing; }
  Phase phase() const { return phase_; }

 private:
  struct Candidate {
    std::uint32_t address, table, frames;
  };

  Phase phase_ = Phase::Idle;
  std::uint32_t previous_held_ = 0, masked_ = 0;
  std::vector<Candidate> candidates_;
  std::uint32_t frames_in_phase_ = 0;
};

}  // namespace bd::game
