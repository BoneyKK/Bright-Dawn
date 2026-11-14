#include <cstdint>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/timeline.h"
#include "nds/timers.h"
#include "nds/video_timing.h"
#include "test.h"

namespace {

using bd::nds::Interrupts;
using bd::nds::Timeline;
using bd::nds::VideoTiming;

// Does something every `period` cycles and notes when.
class Ticker : public Timeline::Source {
 public:
  explicit Ticker(Timeline::Time period) : period_(period), next_(period) {}
  Timeline::Time next_event() const override { return next_; }
  void run_until(Timeline::Time now) override {
    while (next_ != Timeline::kNever && now >= next_) {
      times.push_back(next_);
      seen_at.push_back(now);
      next_ += period_;
    }
  }
  std::vector<Timeline::Time> times, seen_at;

 private:
  Timeline::Time period_, next_;
};

struct Display {
  bd::nds::Arm9Io io;
  Interrupts irq;
  Timeline timeline;
  VideoTiming video{timeline, irq};
  Display() {
    irq.attach(io);
    video.attach(io);
  }
};

struct Counters {
  bd::nds::Arm9Io io;
  Interrupts irq;
  Timeline timeline;
  bd::nds::Timers timers{timeline, irq};
  Counters() {
    irq.attach(io);
    timers.attach(io);
  }
  // Writes timer n's reload value and control in one go, as programs do.
  void start(int n, std::uint32_t reload, std::uint32_t control) {
    io.write(0x04000100 + 4 * static_cast<std::uint32_t>(n), 4, reload | (control << 16));
  }
  std::uint32_t counter(int n) { return io.read(0x04000100 + 4 * static_cast<std::uint32_t>(n), 2); }
};

constexpr std::uint32_t kRun = 0x80, kIrq = 0x40, kCountUp = 0x04;

}  // namespace

TEST(timeline_runs_each_source_at_its_own_time) {
  Timeline timeline;
  Ticker fast(10), slow(25);
  timeline.add(fast);
  timeline.add(slow);
  CHECK(timeline.next_event() == 10);
  timeline.advance(52);
  CHECK(timeline.now() == 52);
  CHECK((fast.times == std::vector<Timeline::Time>{10, 20, 30, 40, 50}));
  CHECK((slow.times == std::vector<Timeline::Time>{25, 50}));
  // Each event was run with the clock at the event, not at the end of the step.
  CHECK(fast.seen_at == fast.times);
  CHECK(slow.seen_at == slow.times);
  CHECK(timeline.next_event() == 60);
  timeline.advance_to(60);
  CHECK(fast.times.back() == 60 && timeline.now() == 60);
  timeline.advance_to(40);  // Never backwards.
  CHECK(timeline.now() == 60);
}

TEST(timeline_without_sources_has_no_events) {
  Timeline timeline;
  CHECK(timeline.next_event() == Timeline::kNever);
  timeline.advance(1000);
  CHECK(timeline.now() == 1000);
}

TEST(timeline_hooks) {
  Timeline timeline;
  int refreshed = 0, rescheduled = 0;
  timeline.refresh();  // Without hooks: nothing happens.
  timeline.rescheduled();
  timeline.on_refresh = [&] { ++refreshed; };
  timeline.on_rescheduled = [&] { ++rescheduled; };
  timeline.refresh();
  timeline.rescheduled();
  CHECK(refreshed == 1 && rescheduled == 1);
}

TEST(video_timing_counts_lines_and_frames) {
  Display d;
  CHECK(d.io.read(0x04000006, 2) == 0);
  d.timeline.advance(VideoTiming::kCyclesPerLine - 1);
  CHECK(d.io.read(0x04000006, 2) == 0);
  d.timeline.advance(1);
  CHECK(d.io.read(0x04000006, 2) == 1);
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 262);
  CHECK(d.io.read(0x04000006, 2) == 262);
  CHECK(d.video.frames() == 1);
  d.timeline.advance_to(VideoTiming::kCyclesPerFrame);  // 263 lines: back to the top.
  CHECK(d.io.read(0x04000006, 2) == 0);
  d.timeline.advance_to(VideoTiming::kCyclesPerFrame * 10);
  CHECK(d.video.frames() == 10 && d.video.line() == 0);
  // 263 lines of 2,130 cycles at 33,513,982 cycles a second: 59.83 frames a second.
  CHECK(VideoTiming::kCyclesPerFrame == 560190);
  CHECK(d.io.unmodelled().empty());
}

TEST(video_timing_blanking_flags) {
  Display d;
  auto status = [&] { return d.io.read(0x04000004, 2); };
  CHECK((status() & 3) == 0);
  d.timeline.advance_to(VideoTiming::kHBlankStart - 1);
  CHECK((status() & 2) == 0);
  d.timeline.advance_to(VideoTiming::kHBlankStart);
  CHECK((status() & 2) != 0);  // The horizontal blank, to the end of the line.
  d.timeline.advance_to(VideoTiming::kCyclesPerLine);
  CHECK((status() & 2) == 0);
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 192 - 1);
  CHECK((status() & 1) == 0);
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 192);
  CHECK((status() & 1) != 0);  // The vertical blank: lines 192 to 261.
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 262 - 1);
  CHECK((status() & 1) != 0);
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 262);
  CHECK((status() & 1) == 0);  // Not in the last line.
  CHECK(d.irq.requested() == 0);  // No interrupt was enabled.
}

TEST(video_timing_interrupts) {
  Display d;
  int blanks = 0;
  d.video.on_vertical_blank = [&] { ++blanks; };
  d.io.write(0x04000004, 2, 0x0008);  // Vertical blank interrupt.
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 192 - 1);
  CHECK(d.irq.requested() == 0 && blanks == 0);
  d.timeline.advance(1);
  CHECK(d.irq.requested() == (1u << Interrupts::kVBlank) && blanks == 1);
  d.io.write(0x04000214, 4, 0xFFFFFFFF);

  d.io.write(0x04000004, 2, 0x0010);  // Horizontal blank interrupt instead: once a line.
  const Timeline::Time line_193 = VideoTiming::kCyclesPerLine * 193;
  d.timeline.advance_to(line_193 + VideoTiming::kHBlankStart - 1);
  CHECK(d.irq.requested() == (1u << Interrupts::kHBlank));  // From line 192's.
  d.io.write(0x04000214, 4, 0xFFFFFFFF);
  d.timeline.advance(1);
  CHECK(d.irq.requested() == (1u << Interrupts::kHBlank));
  d.io.write(0x04000214, 4, 0xFFFFFFFF);

  // A chosen line: 261 has its low eight bits in the top byte and its ninth
  // bit in bit 7.
  d.io.write(0x04000004, 2, 0x0020 | 0x0080 | (0x05 << 8));
  CHECK((d.io.read(0x04000004, 2) & 0xFFB8) == 0x05A0);
  d.timeline.advance_to(VideoTiming::kCyclesPerLine * 261 - 1);
  CHECK(d.irq.requested() == 0 && (d.io.read(0x04000004, 2) & 4) == 0);
  d.timeline.advance(1);
  CHECK(d.irq.requested() == (1u << Interrupts::kVCount));
  CHECK((d.io.read(0x04000004, 2) & 4) != 0);
  d.timeline.advance(VideoTiming::kCyclesPerLine);
  CHECK((d.io.read(0x04000004, 2) & 4) == 0);
  CHECK(blanks == 1);
}

TEST(video_timing_status_flags_cannot_be_written) {
  Display d;
  d.io.write(0x04000004, 2, 0xFFFF);
  CHECK((d.io.read(0x04000004, 2) & 0xFFB8) == 0xFFB8);
  d.io.write(0x04000004, 2, 0x0007);  // The three flags: not writable.
  // Not blanking, whatever was written. The third flag is set, but because
  // the line to match is now 0 and this is line 0.
  CHECK(d.io.read(0x04000004, 2) == 0x0004);
  d.io.write(0x04000004, 2, 0x0107);  // Match line 1 instead: the flag goes, though a one was written to it.
  CHECK(d.io.read(0x04000004, 2) == 0x0100);
}

TEST(timers_count_the_clock) {
  Counters c;
  c.start(0, 0, kRun);
  CHECK(c.counter(0) == 0);
  c.timeline.advance(1000);
  CHECK(c.counter(0) == 1000);
  c.start(1, 0, kRun | 1);  // The clock divided by 64.
  c.timeline.advance(640 + 63);
  CHECK(c.counter(1) == 10);
  c.timeline.advance(1);
  CHECK(c.counter(1) == 11);
  c.start(2, 0, kRun | 2);  // By 256.
  c.start(3, 0, kRun | 3);  // By 1024.
  c.timeline.advance(2048);
  CHECK(c.counter(2) == 8 && c.counter(3) == 2);
  CHECK(c.irq.requested() == 0);
  CHECK(c.io.unmodelled().empty());
}

TEST(timers_reload_and_interrupt_on_overflow) {
  Counters c;
  c.start(0, 0xFFF0, kRun | kIrq);  // Overflows every 16 cycles.
  CHECK(c.counter(0) == 0xFFF0);    // Starting loads the counter.
  CHECK(c.timeline.next_event() == 16);
  c.timeline.advance(15);
  CHECK(c.counter(0) == 0xFFFF && c.irq.requested() == 0);
  c.timeline.advance(1);
  CHECK(c.counter(0) == 0xFFF0);
  CHECK(c.irq.requested() == (1u << Interrupts::kTimer0));
  CHECK(c.timers.overflows(0) == 1);
  c.timeline.advance(16 * 9 + 5);
  CHECK(c.timers.overflows(0) == 10 && c.counter(0) == 0xFFF5);
  // The control half reads back; the counter half reads the count, not the reload value.
  CHECK(c.io.read(0x04000102, 2) == (kRun | kIrq));
}

TEST(timers_reload_value_takes_effect_at_the_next_overflow) {
  Counters c;
  c.start(0, 0xFF00, kRun);
  c.timeline.advance(0x10);
  c.io.write(0x04000100, 2, 0xFFF0);  // A new reload value while running.
  CHECK(c.counter(0) == 0xFF10);      // The count goes on from where it was.
  c.timeline.advance(0xF0);
  CHECK(c.counter(0) == 0xFFF0);      // And reloads with the new value.
}

TEST(timers_stop_and_restart) {
  Counters c;
  c.start(0, 0x1000, kRun);
  c.timeline.advance(5);
  c.io.write(0x04000102, 2, 0);  // Stop: the count is kept.
  c.timeline.advance(500);
  CHECK(c.counter(0) == 0x1005);
  c.io.write(0x04000102, 2, kRun);  // Start again: reloaded.
  CHECK(c.counter(0) == 0x1000);
}

TEST(timers_count_the_overflows_of_the_one_before) {
  Counters c;
  c.start(0, 0xFFFC, kRun);                    // Overflows every 4 cycles.
  c.start(1, 0xFFFE, kRun | kCountUp | kIrq);  // Overflows every 2 of those: every 8 cycles.
  c.start(2, 0, kRun | kCountUp);              // Counts those.
  CHECK(c.timeline.next_event() == 4);         // Timer 0 has no interrupt but feeds timer 1.
  c.timeline.advance(7);
  CHECK(c.counter(1) == 0xFFFF && c.irq.requested() == 0);
  c.timeline.advance(1);
  CHECK(c.counter(1) == 0xFFFE && c.counter(2) == 1);
  CHECK(c.irq.requested() == (1u << (Interrupts::kTimer0 + 1)));
  c.timeline.advance(80);
  CHECK(c.counter(2) == 11 && c.timers.overflows(1) == 11);
}

TEST(timers_that_matter_to_nobody_are_not_events) {
  Counters c;
  c.start(0, 0xFFFC, kRun);  // No interrupt, and nothing counts its overflows.
  CHECK(c.timeline.next_event() == Timeline::kNever);
  c.timeline.advance(10);    // It still counts.
  CHECK(c.counter(0) == 0xFFFE && c.timers.overflows(0) == 2);
}

TEST(timers_count_exactly_across_a_long_step) {
  Counters c;
  c.start(0, 0x8000, kRun | kIrq | 1);  // Clock / 64, 0x8000 steps to an overflow.
  const Timeline::Time period = 0x8000 * 64;
  c.timeline.advance(period * 1000 + 64 * 7 + 13);
  CHECK(c.timers.overflows(0) == 1000);
  CHECK(c.counter(0) == 0x8007);
  c.timeline.advance(51);  // The 13 left-over cycles plus 51 make one more step.
  CHECK(c.counter(0) == 0x8008);
}
