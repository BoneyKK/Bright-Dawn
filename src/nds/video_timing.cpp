#include "nds/video_timing.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

// Display status register.
constexpr u32 kInVBlank = 1u << 0;
constexpr u32 kInHBlank = 1u << 1;
constexpr u32 kLineMatches = 1u << 2;
constexpr u32 kVBlankInterrupt = 1u << 3;
constexpr u32 kHBlankInterrupt = 1u << 4;
constexpr u32 kMatchInterrupt = 1u << 5;
constexpr u32 kWritable = 0xFFB8;  // The interrupt enables and the line to match.

}  // namespace

VideoTiming::VideoTiming(Timeline& timeline, Interrupts& interrupts) : timeline_(timeline), interrupts_(interrupts) {
  timeline_.add(*this);
}

void VideoTiming::attach(Arm9Io& io) {
  io.add(0x04000004, 2, [this] { return read_status(); }, [this](u32 v, u32 m) { write_status(v, m); });
  // Writing the line counter (which moves the displays' timing) is not
  // modelled: writes are ignored.
  io.add(0x04000006, 2,
         [this] {
           timeline_.refresh();
           return static_cast<u32>(line_);
         },
         nullptr);
}

u32 VideoTiming::read_status() {
  timeline_.refresh();
  u32 value = status_;
  // The vertical blank flag is set in lines 192 to 261, not in the last line.
  if (line_ >= kVisibleLines && line_ < kLines - 1) value |= kInVBlank;
  if (timeline_.now() - line_start_ >= kHBlankStart) value |= kInHBlank;
  if (line_ == match_line()) value |= kLineMatches;
  return value;
}

void VideoTiming::write_status(u32 value, u32 mask) {
  timeline_.refresh();
  mask &= kWritable;
  status_ = (status_ & ~mask) | (value & mask);
  timeline_.rescheduled();  // The horizontal blank is an event only while its interrupt is on.
}

Timeline::Time VideoTiming::next_event() const {
  if ((status_ & kHBlankInterrupt) != 0 && !hblank_done_) return line_start_ + kHBlankStart;
  return line_start_ + kCyclesPerLine;
}

void VideoTiming::run_until(Timeline::Time now) {
  for (;;) {
    if (!hblank_done_ && now >= line_start_ + kHBlankStart) {
      hblank_done_ = true;
      if (status_ & kHBlankInterrupt) interrupts_.request(Interrupts::kHBlank);
    }
    if (now < line_start_ + kCyclesPerLine) return;
    start_line();
  }
}

void VideoTiming::start_line() {
  line_start_ += kCyclesPerLine;
  line_ = (line_ + 1) % kLines;
  hblank_done_ = false;
  if (line_ == match_line() && (status_ & kMatchInterrupt)) interrupts_.request(Interrupts::kVCount);
  if (line_ == kVisibleLines) {
    ++frames_;
    if (status_ & kVBlankInterrupt) interrupts_.request(Interrupts::kVBlank);
    if (on_vertical_blank) on_vertical_blank();
  }
}

}  // namespace bd::nds
