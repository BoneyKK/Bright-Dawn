// Console time, and the things that happen at known times.
//
// Time is counted in cycles of the console's system clock (33,513,982 per
// second). It does not pass by itself: whoever runs the processor says how
// much has passed (advance), and when the processor sleeps, time jumps to the
// next event (advance_to). Parts of the hardware that do something at a
// certain time are sources: each says when its next event is and is run up to
// a given time.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace bd::nds {

class Timeline {
 public:
  using Time = std::uint64_t;
  static constexpr Time kNever = ~Time{0};
  static constexpr Time kCyclesPerSecond = 33513982;

  class Source {
   public:
    virtual ~Source() = default;
    // When this source next does something by itself, or kNever.
    virtual Time next_event() const = 0;
    // Brings the source up to time `now`, doing whatever falls due.
    virtual void run_until(Time now) = 0;
  };

  // `source` must outlive the timeline.
  void add(Source& source) { sources_.push_back(&source); }

  Time now() const { return now_; }
  Time next_event() const;

  // Moves time to `time` (not backwards), stopping at every event on the way
  // so that each source acts at its own time.
  void advance_to(Time time);
  void advance(Time cycles) { advance_to(now_ + cycles); }

  // Hooks for whoever keeps time. `refresh` is called by a source before it
  // answers a question that depends on the current time, so that time passed
  // since the last advance can be accounted for. `rescheduled` is called by a
  // source after its next event has changed.
  std::function<void()> on_refresh, on_rescheduled;
  void refresh() {
    if (on_refresh) on_refresh();
  }
  void rescheduled() {
    if (on_rescheduled) on_rescheduled();
  }

 private:
  Time now_ = 0;
  std::vector<Source*> sources_;
};

}  // namespace bd::nds
