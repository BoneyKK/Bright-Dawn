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
  // place of the picture, and the main engine marks where it goes. A frame
  // with a display capture is still drawn here, as the capture needs it.
  void set_hardware_3d(bool on) { hardware_3d_ = on; }
  // Of the frame last completed: whether its main engine's screen has
  // marks for the scene, the main engine's master brightness, and whether
  // the main engine was on the top screen.
  bool frame_has_scene() const { return frame_has_scene_; }
  std::uint16_t frame_brightness() const { return frame_brightness_; }
  bool frame_main_on_top() const { return frame_main_on_top_; }

  DisplayEngine& main_engine() { return main_; }
  DisplayEngine& second_engine() { return second_; }
  std::uint32_t capture_control() const { return capture_control_; }
  std::uint32_t power_control() const { return power_control_; }
  const Statistics& statistics() const { return statistics_; }
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  void capture_line(unsigned line);

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
  bool frame_has_scene_ = false, frame_main_on_top_ = true;
  std::uint16_t frame_brightness_ = 0;
  std::vector<std::uint32_t> top_, bottom_;
  Statistics statistics_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
