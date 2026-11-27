// A frame of a 2D display engine as the graphics hardware draws it
// (app/hardware_3d.h, shaders/engine2d.frag), in place of
// nds::DisplayEngine::draw_line: what each line was drawn with.
//
// The processor only records, at the end of each visible line, the
// engine's registers as draw_line() would use them (Frame2D::lines), and the
// palette and sprite attribute memory where they changed since the line
// before (a copy each time). Video memory is taken as the frame's first
// line begins (Video2D), and then, at each line, the blocks of it that
// changed since the line before: this game writes a bitmap's row for each
// line in the horizontal blank before it.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "nds/video_memory.h"

namespace bd::nds {

struct Frame2D {
  static constexpr int kHeight = 192;
  // A line's registers, as 32-bit words:
  //  0     the control register
  //  1, 2  background control, two a word (0 and 1, 2 and 3)
  //  3-6   scroll of backgrounds 0 to 3: across, and down times 65536
  //  7-10  the matrices of backgrounds 2 and 3: pa | pb << 16, pc | pd << 16
  //  11-14 where the line starts on backgrounds 2 and 3: x, y, x, y (signed)
  //  15    the windows' left and right edges: x1, x2 of window 0, then of 1
  //  16    bits: window 0 on the line, window 1 on it, window 0 inside out,
  //        window 1 inside out; 16: the 3D layer is the graphics
  //        hardware's (the line is marked where it goes)
  //  17    inside of the windows | outside << 16
  //  18    mosaic
  //  19    blend control | blend alpha << 16
  //  20    blend brightness | master brightness << 16
  //  21    which palette copy, 22 which sprite attribute copy
  //  23    unused
  static constexpr int kWords = 24;
  static constexpr std::size_t kCopySize = 0x400;  // A palette or sprite attribute copy: 1 KiB.

  bool by_hardware = false;    // Drawn by the graphics hardware (else by the processor).
  bool second_engine = false;  // The second engine's (else the main engine's).
  std::array<std::uint32_t, kHeight * kWords> lines{};
  std::vector<std::uint8_t> palettes;  // Copies of the engine's palette memory, 1 KiB each.
  std::vector<std::uint8_t> sprites;   // And of its sprite attribute memory.
  // The widescreen HUD's moves for the frame (HudPlan::to_words()): all 0
  // when nothing moves.
  static constexpr std::size_t kHudWords = 64;
  std::array<std::uint32_t, kHudWords> hud{};
};

// Video memory as the 2D engines read it, taken as a frame begins.
struct Video2D {
  // All nine banks, one after another (VideoMemory::kBankStarts).
  std::vector<std::uint8_t> memory;
  // For each page of 16 KiB of `memory`, a number that changes when the page
  // does.
  std::vector<std::uint32_t> versions;
  // Where the engines' addresses are in `memory`: VideoMemory::layout_2d().
  std::vector<std::uint32_t> layout;

  // What changed in `memory` while the lines were drawn, in blocks of
  // kBlockSize: for each block of `memory`, where its changes begin in the
  // list of changes and how many there are (two words); then the list,
  // block by block and, in a block, line by line: the line from which a
  // change is seen (bits 0 to 7) and its block of `changed` (bits 8 to 31).
  // Both empty where nothing changed.
  static constexpr std::uint32_t kBlockSize = 0x100;
  std::vector<std::uint32_t> changes;
  std::vector<std::uint8_t> changed;
};

// Takes Video2D from video memory, a frame at a time.
class Video2DRecorder {
 public:
  // As a frame begins: where the engines' pages are now (false where
  // VideoMemory::layout_2d() is); then, for the engines the graphics
  // hardware is to draw, video memory as it is.
  bool place(const VideoMemory& video);
  void begin(const VideoMemory& video, bool main_engine, bool second_engine);
  // Before each line is recorded: the changes made since the line before,
  // to the pages those engines read.
  void line(const VideoMemory& video, unsigned line);
  // When the frame is done: puts the changes in video_2d(). The number of
  // blocks changed that are left out (beyond kMostChanges).
  std::uint64_t finish();

  const Video2D& video_2d() const { return taken_; }

  static constexpr std::size_t kMostChanges = 0x2000;  // Blocks a frame: 2 MiB.

 private:
  Video2D taken_;
  // VideoMemory::page_writes() when the frame began, and when the last line
  // was recorded.
  std::vector<std::uint32_t> page_writes_taken_;
  std::array<std::uint32_t, VideoMemory::kAllPages> line_writes_{};
  // The pages the engines drawn read; video memory as the lines recorded so
  // far read it; and this frame's changes, with the blocks changed.
  std::array<bool, VideoMemory::kAllPages> shown_pages_{};
  std::vector<std::uint8_t> seen_;
  struct Change {
    std::uint32_t block, line;
  };
  std::vector<Change> changes_;
  std::vector<std::uint8_t> changed_;
  std::uint64_t left_out_ = 0;
};

}  // namespace bd::nds
