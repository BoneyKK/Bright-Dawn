// Real time in, console frames out: the game's steps kept apart from when
// pictures are shown.
//
// The game's own logic advances one fixed step per console frame, as on the
// console: 263 lines of 2130 cycles of the 33,513,982 Hz clock, about
// 59.83 a second. Its code counts frames, so it cannot be fed variable time.
// What is shown, and when, is driven by real time instead: the window adds
// each moment's elapsed time (the delta) to an account, a console frame is
// run whenever a whole step is in it, and pictures are shown at whatever rate
// the screen refreshes, with alpha() saying how far real time is into the
// next step. A renderer that keeps the last two states of a scene can
// interpolate between them by that fraction, so that a screen refreshed
// faster than the game updates a scene still moves smoothly.
//
// Times are in nanoseconds from any fixed start. The account is kept
// exactly (a step is a fraction of a nanosecond longer than 16,715,113), so
// the rate does not drift. When the account holds more than kMostBehind
// steps (the window was dragged, the computer is too slow) the excess is
// dropped and the game runs slower than real time, rather than racing to
// catch up.
#pragma once

#include <cstdint>

namespace bd::platform {

class FrameClock {
 public:
  static constexpr std::uint64_t kCyclesPerFrame = 263 * 2130;
  static constexpr std::uint64_t kCyclesPerSecond = 33'513'982;
  static constexpr std::uint64_t kMostBehind = 4;

  // Real time has reached `now`. The first call only starts the clock.
  void tick(std::uint64_t now);
  // A console frame was run: one step taken out of the account. Taking one
  // that is not there leaves the account empty.
  void step_taken();
  // Starts again from `now`, the account empty, as after a pause: the time
  // the game was paused is not owed to it.
  void restart(std::uint64_t now);

  // Whether a whole step is in the account: time to run a console frame.
  bool step_due() const { return account_ >= kStep; }
  // How far real time is into the next step: 0 up to (not including) 1.
  double alpha() const;
  // Nanoseconds of real time until the next step is due (0 if it is).
  std::uint64_t until_due() const;
  // Times time was dropped because the account held too much.
  std::uint64_t drops() const { return drops_; }

  // A console frame has been run and real time has reached `now`: takes the
  // step and adds the time. Returns whether to show the frame. Not when
  // another step was already due before the time of this frame's run is
  // added: then the game is behind, and showing would only put it further
  // behind, so the next frame is run first. Unless time
  // was dropped within the last kSettle frames (the computer cannot keep up
  // at all, so every frame is shown, the game running slower), or
  // kMostBehind frames in a row have gone unshown.
  static constexpr std::uint64_t kSettle = 60;
  bool frame_done(std::uint64_t now);

 private:
  // The account in 1/kCyclesPerSecond of a nanosecond: one step is then a
  // whole number, kCyclesPerFrame * 10^9.
  static constexpr std::uint64_t kStep = kCyclesPerFrame * 1'000'000'000ull;
  bool started_ = false;
  std::uint64_t last_ = 0;
  std::uint64_t account_ = 0;
  std::uint64_t drops_ = 0;
  std::uint64_t drops_seen_ = 0;
  std::uint64_t frames_since_drop_ = kSettle;
  std::uint64_t unshown_ = 0;
};

}  // namespace bd::platform
