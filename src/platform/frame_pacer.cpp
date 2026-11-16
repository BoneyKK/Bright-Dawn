#include "platform/frame_pacer.h"

namespace bd::platform {

void FramePacer::advance() {
  // One frame is kCyclesPerFrame * 10^9 / kCyclesPerSecond nanoseconds.
  carried_ += kCyclesPerFrame * 1'000'000'000ull;
  deadline_ += carried_ / kCyclesPerSecond;
  carried_ %= kCyclesPerSecond;
}

std::uint64_t FramePacer::frame_shown(std::uint64_t now) {
  const std::uint64_t period = kCyclesPerFrame * 1'000'000'000ull / kCyclesPerSecond;
  if (!started_) {
    started_ = true;
    deadline_ = now;
    carried_ = 0;
  } else if (now > deadline_ + kMostBehind * period) {
    ++restarts_;
    deadline_ = now;
    carried_ = 0;
  }
  advance();
  return deadline_;
}

}  // namespace bd::platform
