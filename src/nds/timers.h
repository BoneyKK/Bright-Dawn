// The four hardware timers of the main processor: 16-bit counters that count
// the system clock (divided by 1, 64, 256 or 1024) or the overflows of the
// timer before them, reload when they overflow, and can raise an interrupt
// when they do.
#pragma once

#include <cstdint>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/timeline.h"

namespace bd::nds {

class Timers : public Timeline::Source {
 public:
  static constexpr int kTimers = 4;

  Timers(Timeline& timeline, Interrupts& interrupts);

  // One 32-bit register per timer at 0x04000100 + 4n: the low half reads the
  // counter and writes the value to reload; the high half is the control.
  void attach(Arm9Io& io);

  Timeline::Time next_event() const override;
  void run_until(Timeline::Time now) override;

  // Overflows so far, per timer.
  std::uint64_t overflows(int timer) const { return timers_[timer].overflows; }

 private:
  struct Timer {
    std::uint32_t reload = 0;
    std::uint32_t control = 0;
    std::uint32_t counter = 0;        // 0 to 0xFFFF.
    Timeline::Time residue = 0;       // Clock cycles counted that do not yet make a step.
    std::uint64_t overflows = 0;
  };
  bool cascades(int n) const;  // Counts the previous timer's overflows.
  std::uint32_t read(int n);
  void write(int n, std::uint32_t value, std::uint32_t mask);

  Timeline& timeline_;
  Interrupts& interrupts_;
  Timer timers_[kTimers];
  Timeline::Time last_ = 0;  // The time the counters are up to date for.
};

}  // namespace bd::nds
