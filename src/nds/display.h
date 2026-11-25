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
#include "nds/frame_2d.h"
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
    // With the graphics hardware's scene: 3D pictures the processor drew
    // too, for the captures written to video memory.
    std::uint64_t pictures_3d_for_captures = 0;
    std::uint64_t captures = 0;           // Display captures finished.
    // Once the game uses 3D, with the graphics hardware asked for: the
    // frames its scene draws, and the frames the processor draws instead
    // (for the reasons in fallbacks()).
    std::uint64_t frames_3d_hardware = 0;
    std::uint64_t frames_3d_processor = 0;
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

  // Widescreen: the main engine draws each line further at its sides too
  // (DisplayEngine::set_sides), where the processor draws it. Of screen `i`
  // (0 top, 1 bottom), those columns as shown (DisplayEngine::side_shown(),
  // laid out as it is; marked DisplayEngine::kCapturedBelow where they show
  // the capture the screen shows), or empty for none.
  void set_sides(bool on) { main_.set_sides(on); }
  const std::vector<std::uint32_t>& screen_sides(int i) const { return screen_sides_[static_cast<std::size_t>(i & 1)]; }

  // The 3D picture drawn by the graphics hardware where it can be: the
  // polygon renderer then makes a scene (PolygonRenderer::build_scene) in
  // place of the picture, and the main engine marks where it goes.
  //
  // A display capture is then made by the graphics hardware too
  // (frame_capture()), at its resolution, whatever it takes: the main
  // engine's picture or the 3D alone, a bank of video memory, or the two
  // blended, of any size, anywhere in its bank. Here it is still written to
  // video memory, for the game, which can read it back (as a texture or a
  // background, as the start of a battle does): with the 3D picture in it,
  // which the processor draws in such a frame too, put in as the graphics
  // hardware puts in its scene. A screen that then shows it (the main
  // engine in display mode 2, the second engine with a copy of it, or a
  // bitmap background turned and made larger or smaller, as the start of a
  // battle and a menu show the field) is marked where it does, with each
  // line's map (CaptureLines), for the graphics hardware to put it in at
  // its resolution; and so is a capture's first source that shows one. A
  // capture that reads a bank holding one of the graphics hardware's reads
  // that one, so that a capture blended with the last (a trail behind what
  // moves) keeps the resolution. One a picture still shows is kept while it
  // is in video memory.
  void set_hardware_3d(bool on) {
    hardware_3d_ = hardware_wanted_ = on;
    fallback_.clear();
  }
  // The graphics hardware was asked for and cannot draw: the processor
  // does, from now on, and its frames are counted under `why`.
  void hardware_3d_failed(const std::string& why) {
    hardware_3d_ = false;
    hardware_wanted_ = true;
    fallback_ = why;
  }
  // The frames the processor drew while the graphics hardware was asked
  // for, by reason.
  const std::map<std::string, std::uint64_t>& fallbacks() const { return fallbacks_; }

  // The 2D pictures drawn by the graphics hardware where they can be
  // (nds/frame_2d.h): with the 3D drawn by it too, an engine's lines are
  // then recorded in place of drawn, unless the processor needs its
  // picture: the main engine's in a frame with a display capture, and both
  // while a capture of the graphics hardware may be on a screen.
  void set_hardware_2d(bool on) { hardware_2d_wanted_ = on; }
  // For checks: the processor draws the pictures the graphics hardware
  // draws too.
  void set_check_2d(bool on) { check_2d_ = on; }
  // Of the frame last completed: screen 0 (top) or 1 (bottom), as the
  // graphics hardware draws it; by_hardware is false where the processor
  // drew it (top_screen(), bottom_screen()).
  const Frame2D& screen_2d(int screen) const { return frames_2d_[(screen & 1) == (main_on_top_ ? 0 : 1) ? 0 : 1]; }
  // Video memory as it was when the frame last completed began, for the
  // engines the graphics hardware draws.
  const Video2D& video_2d() const { return video_2d_.video_2d(); }

  // What the graphics hardware puts in where a screen is marked.
  enum class Fill : std::uint8_t { kNone, kScene, kCapture };
  struct ScreenFill {
    Fill fill = Fill::kNone;
    // A capture put in where the screen is marked DisplayEngine::
    // kCapturedBelow: for kCapture, its number (frame_capture()); for
    // kScene, under the 3D layer, 0 for none.
    std::uint64_t capture = 0;
    std::uint16_t brightness = 0;   // The master brightness of the engine shown.
    CaptureLines lines;             // Where the capture is shown, line by line.
  };
  // Of the frame last completed: 0 the top screen, 1 the bottom screen.
  const ScreenFill& frame_fill(int screen) const { return frame_fills_[screen & 1]; }
  // The capture the graphics hardware makes, of the frame last completed:
  // its number (0 for none, each larger than the last), how it is made,
  // and its first source: the main engine's picture before the display
  // mode and the master brightness, marked for the scene.
  std::uint64_t frame_capture() const { return frame_capture_; }
  // The numbers of the captures kept (0 for none): those the graphics
  // hardware is to keep too.
  std::array<std::uint64_t, 3> kept_numbers() const {
    return {kept_[0].number, kept_[1].number, kept_[2].number};
  }
  const CaptureRecipe& capture_recipe() const { return frame_recipe_; }
  const std::vector<std::uint32_t>& capture_source() const { return capture_source_; }
  // And its sides, as screen_sides(), from the colours before the master
  // brightness; empty for none.
  const std::vector<std::uint32_t>& capture_sides() const { return capture_sides_; }

  DisplayEngine& main_engine() { return main_; }
  DisplayEngine& second_engine() { return second_; }
  std::uint32_t capture_control() const { return capture_control_; }
  std::uint32_t power_control() const { return power_control_; }
  const Statistics& statistics() const { return statistics_; }
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  void capture_line(unsigned line);
  // Marks `screen` where `engine` shows a capture of the graphics hardware
  // still in video memory, and says so in `fill` (with the scene already
  // in `fill`, where the scene does not go).
  // The capture found.
  struct Shown;
  Shown find_capture(const DisplayEngine& engine, std::vector<std::uint32_t>& screen, ScreenFill& fill) const;
  // The main engine's sides (DisplayEngine::side_mixed() or side_shown()),
  // marked where they show `shown`, into `sides`.
  void make_sides(const std::vector<std::uint32_t>& from, const Shown& shown, std::vector<std::uint32_t>& sides) const;

  // Starts this frame's capture by the graphics hardware.
  void start_hardware_capture();

  // A capture made by the graphics hardware: what it put in video memory,
  // six bits a part, where in its bank (256 * 256; kNotCaptured where it
  // put nothing).
  static constexpr std::uint32_t kNotCaptured = 0xFFFFFFFF;
  struct Kept {
    std::uint64_t number = 0;  // 0 while it is made.
    unsigned bank = 0;         // 0 to 3: A to D.
    bool in_memory = false;    // In its bank as the frame began (in_video_memory()).
    std::vector<std::uint32_t> pixels;
  };

  // A kept capture that a picture shows: as it is in its bank (`background`
  // -1), or as background 2 or 3 of the engine shows a bitmap.
  struct Shown {
    const Kept* kept = nullptr;
    int background = -1;
  };
  // The kept capture still in video memory that `colours` (kWidth * kHeight,
  // the colour in the low 18 bits) show most of, at one pixel in many; it
  // counts if three quarters of those it covers are the same. Through a
  // background only with `through_backgrounds`.
  Shown shown_capture(const DisplayEngine& engine, const std::vector<std::uint32_t>& colours,
                      bool through_backgrounds) const;
  // Marks `marked` DisplayEngine::kCapturedBelow where `colours` are those of
  // `shown`, and says where it is in `lines`.
  void put_in(const DisplayEngine& engine, const Shown& shown, const std::vector<std::uint32_t>& colours,
              std::vector<std::uint32_t>& marked, CaptureLines& lines) const;
  // The first row of bank `bank` that background `background` of `engine`
  // shows on `line` as a bitmap 256 pixels across, or -1.
  int bitmap_row(const DisplayEngine& engine, int background, int line, unsigned bank) const;

  // Whether what `kept` put in its bank is still there.
  bool in_video_memory(const Kept& kept) const;
  // Records this frame's line of each engine the graphics hardware draws.
  void record_lines(unsigned line);
  VideoMemory& video_;
  const std::uint8_t* const palette_;  // Both engines' palette memory, the main engine's first,
  const std::uint8_t* const sprites_;  // and their sprite attribute memory.
  const GeometryEngine& geometry_;
  PolygonRenderer& renderer_;
  DisplayEngine main_, second_;
  std::uint32_t capture_control_ = 0, power_control_ = 0;
  bool capturing_ = false;
  std::uint64_t swaps_drawn_ = 0;   // The 3D engine's swap count when the 3D picture was last drawn,
  std::uint64_t swaps_built_ = 0;   // and when the scene was last made.
  bool have_3d_ = false, have_scene_ = false;
  bool hardware_3d_ = false, hardware_wanted_ = false, scene_this_frame_ = false;
  std::string fallback_;  // Why the processor draws, while the graphics hardware is wanted.
  std::map<std::string, std::uint64_t> fallbacks_;
  // Captures by the graphics hardware: this frame's, its place in kept_,
  // and how it is made.
  bool hardware_capture_ = false;
  std::size_t kept_at_ = 0;
  std::array<Kept, 3> kept_;
  bool capture_shown_ = false;  // A screen showed one in the last frame.
  CaptureRecipe recipe_, frame_recipe_;
  std::vector<std::uint32_t> capture_source_;
  std::vector<std::uint32_t> capture_sides_;
  std::array<std::vector<std::uint32_t>, 2> screen_sides_;
  std::uint64_t frame_capture_ = 0;
  std::array<ScreenFill, 2> frame_fills_{};
  // The 2D pictures by the graphics hardware: wanted, and this frame's, by
  // engine (the main engine first); the frame being drawn and the one last
  // completed.
  bool hardware_2d_wanted_ = false, check_2d_ = false;
  std::array<bool, 2> by_hardware_{};
  std::array<Frame2D, 2> recording_, frames_2d_;
  bool main_on_top_ = false;
  Video2DRecorder video_2d_;
  std::vector<std::uint32_t> top_, bottom_;
  Statistics statistics_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
