// The skip of the story at the start of a new game (game/opening_skip.h),
// on a main memory that holds the story's object as the game makes it.
#include <cstdint>
#include <unordered_map>

#include "game/opening_skip.h"
#include "test.h"

namespace {

using bd::game::OpeningSkip;

class Memory : public bd::game::GuestMemory {
 public:
  std::uint32_t read32(std::uint32_t address) const override {
    const auto i = words.find(address);
    return i == words.end() ? 0 : i->second;
  }
  void write32(std::uint32_t address, std::uint32_t value) override {
    words[address] = value;
    ++writes;
  }
  std::unordered_map<std::uint32_t, std::uint32_t> words;
  int writes = 0;
};

constexpr std::uint32_t kStory = 0x0224338c, kTable = 0x0215f570;

// The story's object, as the game has it: a virtual table of code pointers,
// its frames and its movie.
Memory with_story(std::uint32_t movie = OpeningSkip::kNewGameMovie) {
  Memory m;
  m.words[kStory] = kTable;
  m.words[kTable] = 0x021150b4;
  m.words[kTable + 4] = 0x0210dc84;
  m.words[kStory + OpeningSkip::kFramesOffset] = 1000;
  m.words[kStory + OpeningSkip::kMovieOffset] = movie;
  // Another word of 503, as the game's code has, that is no such object.
  m.words[0x0217b004 + OpeningSkip::kMovieOffset] = OpeningSkip::kNewGameMovie;
  m.words[0x0217b004] = 0xe59f02a8;
  m.writes = 0;
  return m;
}

// The game runs a frame of the story.
void story_frame(Memory& m) { ++m.words[kStory + OpeningSkip::kFramesOffset]; }

// Presses Start in the story; returns what the game was given in the frame
// when the skip has found it. The story counts one frame in two.
std::uint32_t press_start(OpeningSkip& skip, Memory& m) {
  const std::uint32_t first = skip.frame(m, OpeningSkip::kStart, true);
  CHECK((first & OpeningSkip::kStart) == (0u));
  CHECK((skip.frame(m, OpeningSkip::kStart, true) & OpeningSkip::kStart) == (0u));
  CHECK(skip.phase() == OpeningSkip::Phase::Finding);
  story_frame(m);
  return skip.frame(m, OpeningSkip::kStart, true);
}

}  // namespace

TEST(opening_skip_marks_the_story_as_the_skippable_one_and_presses_a) {
  Memory m = with_story();
  OpeningSkip skip;
  CHECK((skip.frame(m, 0, true)) == (0u));
  CHECK((press_start(skip, m)) == (OpeningSkip::kA));
  CHECK(skip.phase() == OpeningSkip::Phase::Ending);
  CHECK(skip.skipping());
  CHECK((m.read32(kStory + OpeningSkip::kMovieOffset)) == (OpeningSkip::kSkippableMovie));
  CHECK((m.writes) == (1));
  // A is pressed anew every eight frames; the player's own buttons are not given.
  int presses = 0;
  std::uint32_t before = OpeningSkip::kA;
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t given = skip.frame(m, OpeningSkip::kStart | OpeningSkip::kA, true);
    CHECK((given & ~OpeningSkip::kA) == (0u));
    if (given != 0 && before == 0) ++presses;
    before = given;
  }
  CHECK((presses) == (8));
}

TEST(opening_skip_runs_the_walk_after_the_story_then_gives_the_buttons_back) {
  Memory m = with_story();
  OpeningSkip skip;
  press_start(skip, m);
  // The game ends the story: its object is gone.
  m.words[kStory] = 0x00650067;
  CHECK((skip.frame(m, 0, true)) == (0u));
  CHECK(skip.phase() == OpeningSkip::Phase::Walking);
  for (std::uint32_t i = 1; i < OpeningSkip::kWalkFrames; ++i) {
    CHECK((skip.frame(m, OpeningSkip::kA, true)) == (0u));
    CHECK(skip.skipping());
  }
  // A still held at the end is no new press; once let go, it is given again.
  CHECK((skip.frame(m, OpeningSkip::kA, true)) == (0u));
  CHECK(!skip.skipping());
  CHECK((skip.frame(m, OpeningSkip::kA, true)) == (0u));
  CHECK((skip.frame(m, 0, true)) == (0u));
  CHECK((skip.frame(m, OpeningSkip::kA, true)) == (OpeningSkip::kA));
}

TEST(opening_skip_leaves_memory_alone_without_a_story_running) {
  // An object that looks like the story but runs no frames: nothing is written.
  Memory m = with_story();
  OpeningSkip skip;
  skip.frame(m, OpeningSkip::kStart, true);
  for (std::uint32_t i = 0; i < OpeningSkip::kMostFindingFrames; ++i) CHECK((skip.frame(m, 0, true)) == (0u));
  CHECK(skip.phase() == OpeningSkip::Phase::Idle);
  CHECK((m.writes) == (0));
  // No story at all (the field, another movie): Start goes to the game.
  Memory other = with_story(OpeningSkip::kSkippableMovie);
  OpeningSkip skip2;
  CHECK((skip2.frame(other, OpeningSkip::kStart, true)) == (OpeningSkip::kStart));
  CHECK(skip2.phase() == OpeningSkip::Phase::Idle);
  CHECK((other.writes) == (0));
}

TEST(opening_skip_does_nothing_when_switched_off) {
  Memory m = with_story();
  OpeningSkip skip;
  CHECK((skip.frame(m, OpeningSkip::kStart, false)) == (OpeningSkip::kStart));
  story_frame(m);
  CHECK((skip.frame(m, OpeningSkip::kStart, false)) == (OpeningSkip::kStart));
  CHECK(!skip.skipping());
  CHECK((m.writes) == (0));
}

TEST(opening_skip_gives_the_story_back_its_movie_if_it_does_not_end) {
  Memory m = with_story();
  OpeningSkip skip;
  press_start(skip, m);
  for (std::uint32_t i = 0; i <= OpeningSkip::kMostEndingFrames; ++i) skip.frame(m, 0, true);
  CHECK(skip.phase() == OpeningSkip::Phase::Idle);
  CHECK((m.read32(kStory + OpeningSkip::kMovieOffset)) == (OpeningSkip::kNewGameMovie));
}
