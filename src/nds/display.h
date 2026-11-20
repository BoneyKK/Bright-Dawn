// The DS's display as a whole: the two 2D engines, the 3D picture that the
// main engine can show, the display capture, and the two screens.
//
// A visible line is drawn when its 256 dots are done (when its horizontal
// blank begins), with the registers as they are then, so that a program can
// change them from one line to the next. Both engines draw their line. The
// 3D picture a frame shows is the one made of the polygons handed over at
// the vertical blank before it; it is drawn when the frame's first line
// begins.
//
// The display capture copies the main engine's picture (or the 3D picture
// alone, or a bank of video memory, or two of those blended) into a bank of
// video memory, a line at a time while the lines are drawn, during the frame
// after it is switched on. A program uses it to keep a picture, or, as this
// game does, to show its 3D picture by way of a bank.
//
// One register says which engine's picture is on which screen.
//
// Registers after GBATEK; the capture follows NooDS, except that the bank a
// capture reads as its second source is the one the main engine's control
// register names, as GBATEK has it (NooDS reads the bank being written).
//
// What is not here:
// - The 3D picture is drawn anew only after "swap buffers". The hardware
//   draws it every frame, so textures or drawing registers changed without
//   a swap would show there and do not here.
// - The capture's second source fed from main memory. Noted when asked for.
// - Switching the screens off: the power register's bits are kept, and only
//   the one that swaps the screens is acted on.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/display_engine.h"
#include "nds/geometry_engine.h"
#include "nds/polygon_renderer.h"
#include "nds/video_memory.h"

namespace bd::nds {

class Display {
 public:
  static constexpr int kWidth = DisplayEngine::kWidth, kHeight = DisplayEngine::kHeight;

  static constexpr std::uint32_t kCaptureControl = 0x04000064;  // 32 bits, readable.
  static constexpr std::uint32_t kPowerControl = 0x04000304;    // 16 bits, readable.

  // Bits of the capture control register.
  static constexpr std::uint32_t kCaptureOn = 1u << 31;       // Clears itself when the capture is done.
  static constexpr std::uint32_t kCapture3DOnly = 1u << 24;   // The first source is the 3D picture alone.
  static constexpr std::uint32_t kCaptureFromMainMemory = 1u << 25;
  // Bit of the power control register.
  static constexpr std::uint32_t kMainOnTop = 1u << 15;

  struct Statistics {
    std::uint64_t frames = 0;             // Shown.
    std::uint64_t pictures_3d = 0;        // 3D pictures drawn.
    std::uint64_t captures = 0;           // Display captures finished.
  };

  // `palette` and `sprites` are the two kibibytes each of palette memory and
  // sprite attribute memory: the main engine's half first. Everything passed
  // must outlive the display.
  Display(VideoMemory& video, const std::uint8_t* palette, const std::uint8_t* sprites, const GeometryEngine& geometry,
          PolygonRenderer& renderer);

  void attach(Arm9Io& io);

  // From the display's timing: a line begins (0 to 262); the dots of a
  // visible line are done; the vertical blank begins.
  void line_begins(unsigned line);
  void line_done(unsigned line);
  void vertical_blank();

  // The two screens as last completed: kWidth * kHeight colours of six bits
  // a part each.
  const std::vector<std::uint32_t>& top_screen() const { return top_; }
  const std::vector<std::uint32_t>& bottom_screen() const { return bottom_; }

  // The 3D picture drawn by the graphics hardware where it can be: the
  // polygon renderer then makes a scene (PolygonRenderer::build_scene) in
  // place of the picture, and the main engine marks where it goes.
  //
  // A frame with a display capture is drawn here, as the capture needs the
  // picture, except in the pattern of 3D on both screens: a capture of all
  // of the main engine's picture in each frame while it changes screens.
  // That capture is made by the graphics hardware too (frame_capture()); in
  // video memory it has the 2D pictures only. A screen that then shows it
  // (the main engine in display mode 2, or the second engine with a copy of
  // it) is marked where it does, for the graphics hardware's capture.
  void set_hardware_3d(bool on) { hardware_3d_ = on; }

  // What the graphics hardware puts in where a screen is marked.
  enum class Fill : std::uint8_t { kNone, kScene, kCapture };
  struct ScreenFill {
    Fill fill = Fill::kNone;
    std::uint64_t capture = 0;      // For kCapture: its number (frame_capture()).
    std::uint16_t brightness = 0;   // The master brightness of the engine shown.
  };
  // Of the frame last completed: 0 the top screen, 1 the bottom screen.
  const ScreenFill& frame_fill(int screen) const { return frame_fills_[screen & 1]; }
  // The capture the graphics hardware makes, of the frame last completed:
  // its number (0 for none, each larger than the last), and what it takes:
  // the main engine's picture before the display mode and the master
  // brightness, marked for the scene.
  std::uint64_t frame_capture() const { return frame_capture_; }
  const std::vector<std::uint32_t>& capture_source() const { return capture_source_; }

  DisplayEngine& main_engine() { return main_; }
  DisplayEngine& second_engine() { return second_; }
  std::uint32_t capture_control() const { return capture_control_; }
  std::uint32_t power_control() const { return power_control_; }
  const Statistics& statistics() const { return statistics_; }
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  void capture_line(unsigned line);
  // Marks `screen` where `engine` shows a recent capture of the graphics
  // hardware, and says so in `fill`.
  void find_capture(const DisplayEngine& engine, std::vector<std::uint32_t>& screen, ScreenFill& fill) const;

  // A capture made by the graphics hardware: what it put in video memory,
  // six bits a part.
  struct Kept {
    std::uint64_t number = 0;  // 0 while it is made.
    std::vector<std::uint32_t> pixels;
  };

  VideoMemory& video_;
  const GeometryEngine& geometry_;
  PolygonRenderer& renderer_;
  DisplayEngine main_, second_;
  std::uint32_t capture_control_ = 0, power_control_ = 0;
  bool capturing_ = false;
  std::uint64_t swaps_drawn_ = 0;   // The 3D engine's swap count when the 3D picture was last drawn,
  std::uint64_t swaps_built_ = 0;   // and when the scene was last made.
  bool have_3d_ = false, have_scene_ = false;
  bool hardware_3d_ = false, scene_this_frame_ = false;
  // Captures by the graphics hardware: this frame's, its place in kept_,
  // and the pattern that allows them (the last frame with a capture and
  // where the main engine was).
  bool hardware_capture_ = false;
  std::size_t kept_at_ = 0;
  std::array<Kept, 3> kept_;
  std::uint64_t last_capture_frame_ = 0;
  bool last_main_on_top_ = false;
  std::vector<std::uint32_t> capture_source_;
  std::uint64_t frame_capture_ = 0;
  std::array<ScreenFill, 2> frame_fills_{};
  std::vector<std::uint32_t> top_, bottom_;
  Statistics statistics_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
