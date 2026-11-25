#include "game/opening_skip.h"

namespace bd::game {

namespace {

bool in_main_memory(std::uint32_t address) {
  return address >= OpeningSkip::kMainMemory && address < OpeningSkip::kMainMemory + OpeningSkip::kMainMemorySize;
}

}  // namespace

std::uint32_t OpeningSkip::frame(GuestMemory& memory, std::uint32_t held, bool enabled) {
  const bool start_pressed = (held & kStart) != 0 && (previous_held_ & kStart) == 0;
  previous_held_ = held;
  // Buttons held when the skip ended (or Start that began it) reach the
  // game only once let go: they are no new press for it.
  masked_ &= held;
  const std::uint32_t own = held & ~masked_;
  ++frames_in_phase_;
  const auto go_to = [this](Phase phase) {
    phase_ = phase;
    frames_in_phase_ = 0;
  };

  switch (phase_) {
    case Phase::Idle: {
      if (!enabled || !start_pressed) return own;
      // An object with a virtual table (a pointer to a table of pointers
      // to code in main memory) and movie 503.
      candidates_.clear();
      const std::uint32_t last = kMainMemory + kMainMemorySize - kMovieOffset - 4;
      for (std::uint32_t a = kMainMemory; a <= last; a += 4) {
        if (memory.read32(a + kMovieOffset) != kNewGameMovie) continue;
        const std::uint32_t table = memory.read32(a);
        if (!in_main_memory(table) || (table & 3) != 0 || table + 4 >= kMainMemory + kMainMemorySize) continue;
        const std::uint32_t method = memory.read32(table + 4);
        if (!in_main_memory(method)) continue;
        candidates_.push_back({a, table, memory.read32(a + kFramesOffset)});
      }
      if (candidates_.empty()) return own;
      masked_ |= kStart;
      go_to(Phase::Finding);
      return own & ~kStart;
    }
    case Phase::Finding: {
      // The story counts its frames (one in two of the console's): its
      // count goes up within a few frames of Start.
      std::uint32_t story = 0;
      for (const Candidate& c : candidates_) {
        const std::uint32_t frames = memory.read32(c.address + kFramesOffset);
        if (memory.read32(c.address) == c.table && memory.read32(c.address + kMovieOffset) == kNewGameMovie &&
            frames > c.frames && frames - c.frames <= frames_in_phase_) {
          story = c.address;
          break;
        }
      }
      if (story == 0) {
        if (frames_in_phase_ < kMostFindingFrames) return own;
        candidates_.clear();
        go_to(Phase::Idle);
        return own;
      }
      candidates_.clear();
      memory.write32(story + kMovieOffset, kSkippableMovie);
      go_to(Phase::Pressing);
      return kA;
    }
    case Phase::Pressing:
      if (frames_in_phase_ < kPressFrames) return kA;
      // The game ends the story and goes to its title; the player goes on
      // from there.
      go_to(Phase::Idle);
      masked_ = held;
      return 0;
  }
  return own;
}

}  // namespace bd::game
