#include "nds/timers.h"

#include <algorithm>

namespace bd::nds {
namespace {

using u32 = std::uint32_t;
using Time = Timeline::Time;

constexpr u32 kPrescaler = 3;     // Bits 0-1: divide the clock by 1, 64, 256, 1024.
constexpr u32 kCountUp = 1u << 2; // Count the previous timer's overflows instead of the clock.
constexpr u32 kInterrupt = 1u << 6;
constexpr u32 kRunning = 1u << 7;
constexpr u32 kControlBits = kPrescaler | kCountUp | kInterrupt | kRunning;
constexpr unsigned kShift[4] = {0, 6, 8, 10};
constexpr u32 kRange = 0x10000;

}  // namespace

Timers::Timers(Timeline& timeline, Interrupts& interrupts) : timeline_(timeline), interrupts_(interrupts) {
  timeline_.add(*this);
}

void Timers::attach(Arm9Io& io) {
  for (int n = 0; n < kTimers; ++n) {
    io.add(0x04000100 + 4 * static_cast<u32>(n), 4, [this, n] { return read(n); },
           [this, n](u32 v, u32 m) { write(n, v, m); });
  }
}

bool Timers::cascades(int n) const { return n > 0 && (timers_[n].control & kCountUp) != 0; }

u32 Timers::read(int n) {
  timeline_.refresh();
  run_until(timeline_.now());
  return timers_[n].counter | (timers_[n].control << 16);
}

void Timers::write(int n, u32 value, u32 mask) {
  timeline_.refresh();
  run_until(timeline_.now());
  Timer& t = timers_[n];
  if (mask & 0xFFFFu) t.reload = (t.reload & ~mask & 0xFFFFu) | (value & mask & 0xFFFFu);
  if (mask & 0xFFFF0000u) {
    const u32 before = t.control;
    const u32 control_mask = (mask >> 16) & kControlBits;
    t.control = (t.control & ~control_mask) | ((value >> 16) & control_mask);
    // Starting a timer loads its counter and begins a fresh count.
    if ((before & kRunning) == 0 && (t.control & kRunning) != 0) {
      t.counter = t.reload;
      t.residue = 0;
    }
  }
  timeline_.rescheduled();
}

Time Timers::next_event() const {
  Time next = Timeline::kNever;
  for (int n = 0; n < kTimers; ++n) {
    const Timer& t = timers_[n];
    if ((t.control & kRunning) == 0 || cascades(n)) continue;
    // Its overflow matters if it raises an interrupt itself or feeds a timer
    // that counts its overflows.
    const bool feeds = n + 1 < kTimers && (timers_[n + 1].control & kRunning) != 0 && cascades(n + 1);
    if ((t.control & kInterrupt) == 0 && !feeds) continue;
    const unsigned shift = kShift[t.control & kPrescaler];
    const Time steps = kRange - t.counter;
    next = std::min(next, last_ + (steps << shift) - t.residue);
  }
  return next;
}

void Timers::run_until(Time now) {
  if (now <= last_) return;
  const Time elapsed = now - last_;
  last_ = now;
  std::uint64_t previous_overflows = 0;
  for (int n = 0; n < kTimers; ++n) {
    Timer& t = timers_[n];
    if ((t.control & kRunning) == 0) {
      previous_overflows = 0;
      continue;
    }
    std::uint64_t steps;
    if (cascades(n)) {
      steps = previous_overflows;
    } else {
      const unsigned shift = kShift[t.control & kPrescaler];
      const Time total = t.residue + elapsed;
      steps = total >> shift;
      t.residue = total & ((Time{1} << shift) - 1);
    }
    std::uint64_t overflows = 0;
    std::uint64_t value = t.counter + steps;
    if (value >= kRange) {
      const std::uint64_t period = kRange - t.reload;  // Steps from a reload to the next overflow.
      overflows = 1 + (value - kRange) / period;
      value = t.reload + (value - kRange) % period;
    }
    t.counter = static_cast<u32>(value);
    t.overflows += overflows;
    if (overflows != 0 && (t.control & kInterrupt)) interrupts_.request(Interrupts::kTimer0 + static_cast<unsigned>(n));
    previous_overflows = overflows;
  }
}

}  // namespace bd::nds
