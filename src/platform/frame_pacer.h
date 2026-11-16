// Keeps frames shown at the console's rate: about 59.83 a second, one every
// 263 lines of 2130 cycles of the 33,513,982 Hz system clock.
//
// Times are in nanoseconds from any fixed start. The deadlines are kept
// exactly, the fraction of a nanosecond carried from frame to frame, so the
// rate does not drift. A run that falls more than kMostBehind frames behind
// (the window was dragged, the machine was busy) starts counting again from
// the moment it noticed, rather than racing to catch up.
#pragma once

#include <cstdint>

namespace bd::platform {

class FramePacer {
 public:
  static constexpr std::uint64_t kCyclesPerFrame = 263 * 2130;
  static constexpr std::uint64_t kCyclesPerSecond = 33'513'982;
  static constexpr std::uint64_t kMostBehind = 4;

  // A frame has been shown at `now`. Returns when the next should be: wait
  // until then if it is later than now.
  std::uint64_t frame_shown(std::uint64_t now);

  // Frames after which the count started again.
  std::uint64_t restarts() const { return restarts_; }

 private:
  bool started_ = false;
  std::uint64_t deadline_ = 0;
  std::uint64_t carried_ = 0;  // In 1/kCyclesPerSecond of a nanosecond.
  std::uint64_t restarts_ = 0;
  void advance();
};

}  // namespace bd::platform
