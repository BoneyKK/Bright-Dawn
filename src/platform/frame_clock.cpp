#include "platform/frame_clock.h"

namespace bd::platform {

void FrameClock::tick(std::uint64_t now) {
  if (!started_) {
    started_ = true;
    last_ = now;
    return;
  }
  if (now <= last_) return;  // Time that runs backwards adds nothing.
  // A long pause is dropped anyway; bounding it first keeps the product
  // below overflow (a second is about 3.4 * 10^16 here).
  std::uint64_t delta = now - last_;
  last_ = now;
  constexpr std::uint64_t kLongest = 1'000'000'000ull;
  if (delta > kLongest) delta = kLongest;
  account_ += delta * kCyclesPerSecond;
  if (account_ > kMostBehind * kStep) {
    account_ = kStep;
    ++drops_;
  }
}

void FrameClock::restart(std::uint64_t now) {
  started_ = true;
  last_ = now;
  account_ = 0;
  unshown_ = 0;
}

void FrameClock::step_taken() { account_ = account_ >= kStep ? account_ - kStep : 0; }

bool FrameClock::frame_done(std::uint64_t now) {
  step_taken();
  // Behind only if the next step was due before this frame's own run time is
  // counted. Counting it first hid frames that were on time: with vsync the
  // run time plus the wait for the screen often makes up one step, so every
  // second frame went unshown although the game kept the console's speed.
  const bool behind = step_due();
  tick(now);
  if (drops_ != drops_seen_) {
    drops_seen_ = drops_;
    frames_since_drop_ = 0;
  } else if (frames_since_drop_ < kSettle) {
    ++frames_since_drop_;
  }
  if (behind && frames_since_drop_ >= kSettle && unshown_ < kMostBehind) {
    ++unshown_;
    return false;
  }
  unshown_ = 0;
  return true;
}

double FrameClock::alpha() const {
  const std::uint64_t part = account_ % kStep;
  return static_cast<double>(part) / static_cast<double>(kStep);
}

std::uint64_t FrameClock::until_due() const {
  if (account_ >= kStep) return 0;
  // Rounded up, so that waiting that long makes the step due.
  return (kStep - account_ + kCyclesPerSecond - 1) / kCyclesPerSecond;
}

}  // namespace bd::platform
