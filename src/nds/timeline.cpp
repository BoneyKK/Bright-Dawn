#include "nds/timeline.h"

#include <algorithm>

namespace bd::nds {

Timeline::Time Timeline::next_event() const {
  Time next = kNever;
  for (const Source* source : sources_) next = std::min(next, source->next_event());
  return next;
}

void Timeline::advance_to(Time time) {
  for (;;) {
    const Time next = next_event();
    if (next > time) break;
    // An event at or before the present is run at the present.
    now_ = std::max(now_, next);
    for (Source* source : sources_) source->run_until(now_);
  }
  if (time > now_) {
    now_ = time;
    for (Source* source : sources_) source->run_until(now_);
  }
}

}  // namespace bd::nds
