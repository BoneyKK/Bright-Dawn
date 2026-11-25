// One of the DS's two 2D display engines: what turns backgrounds, sprites,
// the 3D picture and the registers into a line of the screen.
//
// The console has two, one for each screen. Each draws up to four
// backgrounds and 128 sprites, a line at a time, and mixes them:
// - A background is a grid of tiles (16 or 256 colours a tile, scrolled), or
//   a grid of tiles or a bitmap seen through a matrix (turned, scaled), by
//   the mode the engine is in and the background's own control register.
//   Background 0 of the main engine can instead be the 3D picture.
// - A sprite is a rectangle of 8 to 64 pixels a side, of tiles (16 or 256
//   colours) or a bitmap, mirrored or seen through a matrix, placed anywhere.
//   Sprites are one layer; among them the lower number is on top, unless
//   priorities say otherwise. A sprite can ask to be blended with what is
//   below it, or be a window instead of being drawn.
// - Every background and sprite has a priority. For each pixel the two
//   topmost layers are kept, so that the top one can be blended with the
//   one below.
// - Two windows (rectangles), the window that sprites make, and the outside
//   of all, each say which layers show inside them and whether effects
//   apply.
// - Effects: alpha blending of the top layer with the one below, or
//   brightening or darkening the top layer. A 3D pixel is blended by its own
//   alpha.
// - What is shown is that picture, or white (display off), or, for the main
//   engine, a bank of video memory shown directly. Last, the master
//   brightness fades the whole screen towards white or black.
//
// Colours are six bits a part here, as the hardware mixes them: a 2D colour
// of five bits a part is doubled.
//
// The registers are GBATEK's; the drawing follows NooDS's 2D engine step by
// step.
//
// What is not here:
// - Mosaic, except its vertical part on tiled backgrounds and on sprites (as
//   in NooDS).
// - The alpha of bitmap sprites: they are drawn solid.
// - The display fed from main memory (display mode 3). Noted when asked for.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/frame_2d.h"
#include "nds/video_memory.h"

namespace bd::nds {

class DisplayEngine {
 public:
  enum Which : unsigned { kMain = 0, kSub = 1 };
  static constexpr int kWidth = 256, kHeight = 192;

  // Registers, as offsets from the engine's base (0x04000000 for the main
  // engine, 0x04001000 for the second).
  static constexpr std::uint32_t kBases[2] = {0x04000000, 0x04001000};
  static constexpr std::uint32_t kControl = 0x00;          // 32 bits, readable.
  static constexpr std::uint32_t kBackgroundControl = 0x08;  // Four of 16 bits, readable.
  static constexpr std::uint32_t kScroll = 0x10;           // Four pairs of 16 bits: across, down.
  static constexpr std::uint32_t kMatrix = 0x20;           // For backgrounds 2 and 3, 16 bytes each:
                                                           // four 16-bit numbers and two reference points.
  static constexpr std::uint32_t kWindowAcross = 0x40;     // Two of 16 bits; then down, two of 16 bits.
  static constexpr std::uint32_t kWindowInside = 0x48;     // Readable.
  static constexpr std::uint32_t kWindowOutside = 0x4A;    // Readable.
  static constexpr std::uint32_t kMosaic = 0x4C;
  static constexpr std::uint32_t kBlendControl = 0x50;     // Readable.
  static constexpr std::uint32_t kBlendAlpha = 0x52;       // Readable.
  static constexpr std::uint32_t kBlendBrightness = 0x54;
  static constexpr std::uint32_t kMasterBrightness = 0x6C;  // Readable.

  // Bits of the control register.
  static constexpr std::uint32_t kBackground0Is3D = 1u << 3;
  static constexpr std::uint32_t kShowBackground0 = 1u << 8;  // And the next three for backgrounds 1 to 3.
  static constexpr std::uint32_t kShowSprites = 1u << 12;
  static constexpr std::uint32_t kWindow0 = 1u << 13, kWindow1 = 1u << 14, kSpriteWindow = 1u << 15;
  static constexpr unsigned kDisplayModeShift = 16;        // 0 off, 1 the picture, 2 a bank, 3 from main memory.
  static constexpr std::uint32_t kExtendedPalettes = 1u << 30;

  static constexpr std::uint32_t kSpriteMemorySize = 0x400;

  // `palette` is this engine's kibibyte of palette memory: 256 colours for
  // backgrounds, then 256 for sprites. `sprites` is its kibibyte of sprite
  // attributes: eight bytes a sprite. They and `video` must outlive the
  // engine.
  DisplayEngine(Which which, const VideoMemory& video, const std::uint8_t* palette, const std::uint8_t* sprites);

  void attach(Arm9Io& io);

  // The 3D picture for background 0 (main engine): kWidth * kHeight pixels
  // of six bits a part with alpha from bit 18, or null for none.
  void set_3d_picture(const std::vector<std::uint32_t>* picture) { picture_3d_ = picture; }
  // The 3D picture is drawn by the graphics hardware (nds/hardware_scene.h):
  // the screen is mixed as if the 3D layer were empty, and each pixel where
  // the 3D layer is on top is marked with how a 3D pixel is mixed there
  // (the marks below, over the colour). The other layers are mixed with it
  // as if it were not there.
  void set_hardware_3d(bool on) { hardware_3d_ = on; }
  static constexpr std::uint32_t kMark3D = 1u << 31;
  static constexpr unsigned kMarkEffectShift = 24;  // Two bits: 0 as it is, 1 blended by its alpha, 2 brighter, 3 darker.
  static constexpr unsigned kMarkFactorShift = 26;  // Five bits: sixteenths, for brighter and darker.
  // Not made here: on a screen with the 3D scene, a pixel whose colour
  // without the 3D layer is a display capture's of the graphics hardware
  // (nds::Display::find_capture), which has its own copy at its resolution.
  static constexpr std::uint32_t kCapturedBelow = 1u << 23;
  // Where a layer on top is blended with the 3D layer just below it (the
  // 3D as the second target): this mark (no kMark3D), the weight of the
  // layer on top in sixteenths at kMarkFactorShift, the 3D's at
  // kMarkBelowWeightShift (five bits). The colour is the top layer's, not
  // blended and without the master brightness.
  static constexpr std::uint32_t kMark3DBelow = 1u << kMarkEffectShift;
  static constexpr unsigned kMarkBelowWeightShift = 18;
  static bool is_3d_below(std::uint32_t pixel) { return (pixel >> kMarkEffectShift & 0x83) == 1; }
  // A pixel of the screen as the graphics hardware takes it (red in the
  // lowest byte): six bits a part made eight, the marks as the alpha, and
  // the low bits that the marks need: kCapturedBelow as red's lowest bit,
  // the 3D's weight of kMark3DBelow in red's second bit (its top bit) and
  // the two lowest of green and blue.
  static std::uint32_t for_hardware(std::uint32_t pixel, bool marks);
  std::uint16_t master_brightness() const { return master_brightness_; }

  // A frame begins: the reference points of the matrix backgrounds start
  // over.
  void start_frame();
  // A line begins (0 to 262): the windows' upper and lower edges take effect.
  void start_line(int line);
  // Draws a visible line (0 to 191).
  void draw_line(int line);
  // In place of draw_line(), for the graphics hardware (nds/frame_2d.h):
  // records into `frame` what the line would be drawn with, naming the
  // copies of palette and sprite attribute memory it reads, and moves on
  // as draw_line() does. screen() and the rest are then not drawn. Before
  // draw_line() for the same line (for checks), with `move_on` false.
  void record_line(int line, Frame2D& frame, std::uint32_t palette_copy, std::uint32_t sprite_copy,
                   bool move_on = true);
  // Which backgrounds go through a matrix on a line drawn with `control`:
  // bit 0 for background 2, bit 1 for background 3.
  static unsigned matrix_backgrounds(std::uint32_t control);

  // A line's background 2 or 3 drawn as a bitmap of colours as such (16
  // bits a pixel), through its matrix: where its pixels are, from what
  // place (in 1/256ths of a pixel) and with what matrix. For nds::Display,
  // to find a display capture shown so. `on` is false where it was not.
  struct BitmapLine {
    bool on = false;
    bool repeated = false;
    std::uint32_t data = 0;
    std::int32_t x0 = 0, y0 = 0;
    std::int16_t pa = 0, pb = 0, pc = 0, pd = 0;
    std::uint16_t width = 0, height = 0;
  };
  const BitmapLine& bitmap_line(unsigned background, int line) const {
    return bitmap_lines_[background & 1][static_cast<std::size_t>(line)];
  }

  // Widescreen: each line is drawn kSideColumns further on each side too,
  // as the layers go on there: a background through its matrix, or its map,
  // or the wider picture it is filled from (set_main_memory), else mirrored
  // where it repeats and is scrolled across; a fixed one (a text box, say)
  // as its pixel at the screen's edge; sprites that reach the screen. With
  // the marks of the 3D (set_hardware_3d), the windows as at the edge, and
  // the effects; without the processor's 3D picture.
  static constexpr int kSideColumns = 44;
  void set_sides(bool on) { sides_ = on; }
  bool sides() const { return sides_; }
  // Of each line, the kSideColumns left of the screen then those right of
  // it (kHeight * 2 * kSideColumns): the colours mixed, with the marks
  // (before the display mode and the master brightness, as mixed_line());
  // with the master brightness; and the top layer of each (Dot::layer).
  const std::vector<std::uint32_t>& side_mixed() const { return side_mixed_; }
  const std::vector<std::uint32_t>& side_shown() const { return side_shown_; }
  const std::vector<std::uint8_t>& side_layers() const { return side_layers_; }
  // Main memory, for the sides: a background that the game fills from a
  // wider picture there as it scrolls (a battle's backdrop, 512 across)
  // shows that picture at the sides, not a copy of the screen. Null: none.
  void set_main_memory(const std::uint8_t* memory, std::size_t size) {
    main_memory_ = memory;
    main_size_ = size;
  }

  // The screen as shown: kWidth * kHeight colours of six bits a part.
  const std::vector<std::uint32_t>& screen() const { return screen_; }
  // The line last drawn before the display mode and the master brightness:
  // what the display capture takes.
  const std::array<std::uint32_t, kWidth>& mixed_line() const { return mixed_; }
  // The marks of the line last drawn (see set_hardware_3d), or null if it
  // has none.
  const std::array<std::uint32_t, kWidth>* line_marks() const { return marked_ ? &marks_ : nullptr; }
  // The screen as shown, before the master brightness.
  const std::vector<std::uint32_t>& unlit_screen() const { return unlit_; }

  std::uint32_t control() const { return control_; }
  // What was asked for that is not carried out, with how many lines.
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  // One layer's pixel.
  struct Dot {
    std::uint32_t colour = 0;   // Six bits a part.
    std::int8_t priority = 4;   // A sprite's is one less than written: it is above a background of the same.
    std::uint8_t layer = 5;     // 0 to 3 a background, 4 sprites, 5 the backdrop.
    bool from_3d = false;
    std::uint8_t alpha = 0;     // Of a 3D pixel: six bits.
    bool translucent = false;   // A sprite that asks to be blended.
  };

  std::uint32_t read(std::uint32_t address, unsigned size) const { return video_.peek(address, size); }
  // A colour of the standard palettes, six bits a part: 0 to 255 the
  // backgrounds', 256 to 511 the sprites'.
  std::uint32_t colour_of(unsigned index) const;
  unsigned shown_inside(int x) const;             // The six bits of whichever window `x` is in.
  // The layers of a line into top_ and below_ (and marks_), blended, from
  // column origin_ on, columns_ of them; `side`: a side's (see set_sides).
  void draw_layers(int line, bool side);
  // Pixel `x` (of the screen; any) of tiled background `background` on
  // `line`: false where it has none.
  bool tile_pixel(unsigned background, int line, int x, std::uint32_t& colour);
  // A column past a side of the screen mirrored into it.
  static int mirrored(int x);
  // Where matrix background `n` starts (one step before the first pixel)
  // for the columns being drawn, and its step across; at a side, mirrored
  // if `mirror` (see set_sides).
  void matrix_start(unsigned n, bool mirror, std::int32_t& at_x, std::int32_t& at_y, std::int32_t& step_x,
                    std::int32_t& step_y) const;
  // The wider picture that matrix background `n` (tiles, 256 across, that
  // repeat) is filled from (see set_main_memory): a picture of
  // kStreamedWidth by kStreamedHeight, a byte a pixel, whose columns of 8
  // the game puts in as columns of 16 tiles (column c's tiles 16c to
  // 16c + 15, top down), 32 of them in a ring, so that the background's x
  // (not repeated) is the picture's, modulo its width. Whether there is one
  // (checked against the screen once a frame; looked for when it fails).
  bool streamed_source(unsigned n);
  static constexpr int kStreamedWidth = 512, kStreamedHeight = 128;
  void place(unsigned background, int x, std::uint32_t colour, bool from_3d = false, std::uint8_t alpha = 0);
  void draw_sprites(int line, bool window_pass);
  void draw_3d(int line);
  void mark_3d();
  void draw_tiles(unsigned background, int line);
  void draw_matrix_tiles(unsigned background, int line);
  void draw_extended(unsigned background, int line);
  void draw_large(unsigned background, int line);
  void blend(int line);

  const Which which_;
  const VideoMemory& video_;
  const std::uint8_t* const palette_;
  const std::uint8_t* const sprites_;
  const std::vector<std::uint32_t>* picture_3d_ = nullptr;
  bool hardware_3d_ = false;
  bool marked_ = false;                       // The line has marks.
  std::array<std::uint32_t, 256> marks_{};
  const std::uint32_t vram_;  // Where this engine's backgrounds are in video memory.

  std::uint32_t control_ = 0;
  std::array<std::uint16_t, 4> background_control_{}, scroll_x_{}, scroll_y_{};
  // For backgrounds 2 and 3: the matrix, the reference point as written and
  // as it stands on the current line.
  std::array<std::int16_t, 2> pa_{}, pb_{}, pc_{}, pd_{};
  std::array<std::int32_t, 2> reference_x_{}, reference_y_{}, line_x_{}, line_y_{};
  // The windows: left and right edge (in order), whether the inside is the
  // outside (edges written the other way round), top and bottom edge, and
  // whether the current line is between them.
  std::array<std::uint8_t, 2> window_x1_{}, window_x2_{}, window_y1_{}, window_y2_{};
  std::array<bool, 2> window_flipped_{}, window_on_line_{};
  std::uint16_t window_inside_ = 0, window_outside_ = 0, mosaic_ = 0;
  std::uint16_t blend_control_ = 0, blend_alpha_ = 0, master_brightness_ = 0;
  std::uint8_t blend_brightness_ = 0;

  std::array<std::array<BitmapLine, kHeight>, 2> bitmap_lines_{};  // Of backgrounds 2 and 3.
  // The columns being drawn: from origin_ (of the screen), columns_ of them
  // (into top_ and the rest from their start).
  int origin_ = 0, columns_ = kWidth;
  bool sides_ = false;
  std::vector<std::uint32_t> side_mixed_, side_shown_;
  std::vector<std::uint8_t> side_layers_;
  const std::uint8_t* main_memory_ = nullptr;
  std::size_t main_size_ = 0;
  unsigned frames_ = 0;  // Frames begun, for streamed_.
  struct Streamed {
    bool found = false;
    std::size_t base = 0;       // In main memory, its top left pixel.
    unsigned checked = ~0u;     // The frame it was checked on.
    unsigned next_search = 0;   // The frame it may be looked for again.
  };
  std::array<Streamed, 2> streamed_{};
  std::array<Dot, kWidth> top_{}, below_{};
  std::array<bool, kWidth> sprite_window_{};
  std::array<std::uint32_t, kWidth> mixed_{};
  std::vector<std::uint32_t> screen_;
  std::vector<std::uint32_t> unlit_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
