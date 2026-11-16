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

  // A frame begins: the reference points of the matrix backgrounds start
  // over.
  void start_frame();
  // A line begins (0 to 262): the windows' upper and lower edges take effect.
  void start_line(int line);
  // Draws a visible line (0 to 191).
  void draw_line(int line);

  // The screen as shown: kWidth * kHeight colours of six bits a part.
  const std::vector<std::uint32_t>& screen() const { return screen_; }
  // The line last drawn before the display mode and the master brightness:
  // what the display capture takes.
  const std::array<std::uint32_t, kWidth>& mixed_line() const { return mixed_; }

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
  void place(unsigned background, int x, std::uint32_t colour, bool from_3d = false, std::uint8_t alpha = 0);
  void draw_sprites(int line, bool window_pass);
  void draw_3d(int line);
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

  std::array<Dot, kWidth> top_{}, below_{};
  std::array<bool, kWidth> sprite_window_{};
  std::array<std::uint32_t, kWidth> mixed_{};
  std::vector<std::uint32_t> screen_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
