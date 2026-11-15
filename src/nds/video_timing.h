// The timing of the DS's displays: which line is being drawn, the blanking
// periods, and the interrupts they raise. Nothing is drawn here.
//
// A frame is 263 lines of 2,130 system clock cycles: 192 visible lines, then
// the vertical blank. Within a line, the horizontal blank begins after the
// 256 visible dots.
#pragma once

#include <cstdint>
#include <functional>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/timeline.h"

namespace bd::nds {

class VideoTiming : public Timeline::Source {
 public:
  static constexpr unsigned kLines = 263;
  static constexpr unsigned kVisibleLines = 192;
  static constexpr Timeline::Time kCyclesPerLine = 2130;
  static constexpr Timeline::Time kHBlankStart = 1536;  // 256 dots of 6 cycles.
  static constexpr Timeline::Time kCyclesPerFrame = kCyclesPerLine * kLines;

  VideoTiming(Timeline& timeline, Interrupts& interrupts);

  // Registers: display status (0x04000004) and the line counter (0x04000006).
  void attach(Arm9Io& io);

  Timeline::Time next_event() const override;
  void run_until(Timeline::Time now) override;

  unsigned line() const { return line_; }
  // Vertical blanks begun so far.
  std::uint64_t frames() const { return frames_; }
  // Called at the start of each vertical blank.
  std::function<void()> on_vertical_blank;
  // Called at the start of the horizontal blank of each visible line, while
  // someone is watching for it.
  std::function<void()> on_horizontal_blank;
  // Whether the horizontal blank of visible lines is to be reported through
  // on_horizontal_blank. Each one is then an event of its own, which costs
  // time, so it is off unless something waits for it.
  void watch_horizontal_blank(bool watch);
  bool watching_horizontal_blank() const { return hblank_watched_; }

 private:
  std::uint32_t read_status();
  void write_status(std::uint32_t value, std::uint32_t mask);
  void start_line();
  unsigned match_line() const { return ((status_ >> 8) & 0xFFu) | ((status_ & 0x80u) << 1); }

  Timeline& timeline_;
  Interrupts& interrupts_;
  unsigned line_ = 0;
  Timeline::Time line_start_ = 0;  // When the current line began.
  bool hblank_done_ = false;       // The horizontal blank of this line has begun.
  bool hblank_watched_ = false;
  std::uint32_t status_ = 0;       // The writable bits of the status register.
  std::uint64_t frames_ = 0;
};

}  // namespace bd::nds
