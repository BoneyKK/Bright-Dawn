#pragma once

// The widescreen HUD: a rendering function of our own. In widescreen the
// main engine's screen is wider than the DS's, but the game places its HUD
// on the DS's 256 columns. This finds the HUD's elements in each frame by
// what they are on the screen (where they are, their size, their layer, and
// what else is on the screen), and draws them at the wide screen's edges:
// a box or an icon moves to the wide corner or edge; a bar (a text box, the
// ring of commands) is drawn longer, its middle going on into the sides and
// its ends moved to the new ends, never stretched. Only the drawing changes:
// the game, its memory and its sprite attributes stay as they are, and the
// game still sees the DS's 256 by 192 screen. A touch on a moved element
// goes to the game at the element's place on the DS's screen (touches()).
//
// An element is moved only if all of it is found as expected; else that
// frame it is drawn where the game put it (centred). Off (the setting
// "Widescreen HUD: Centred", or no widescreen), nothing here is used and
// the picture is as without it.
//
// The facts below were measured on the screen (positions in DS pixels);
// they are not data from the game.

#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>

#include "nds/geometry_engine.h"

namespace bd::nds {

// What the widescreen HUD moves in a frame's 2D picture, as the 2D engines
// draw it (DisplayEngine on the processor; engine2d.frag on the graphics
// hardware, through Frame2D::hud).
struct HudPlan {
  static constexpr unsigned kMostAreas = 8;
  static constexpr unsigned kMostTouches = 16;
  // An area of the backgrounds `layers` (a bit each): its pixels from x0 up
  // to x1, y0 up to y1, are drawn dx further across, and none are left
  // where they were.
  struct Area {
    std::int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0;
    std::uint8_t layers = 0;
  };
  // A moved element's place on the wide screen, x0 up to x1 (DS pixels from
  // the DS screen's left edge: less than 0 at the left side), and how far it
  // was moved: a touch there is at x - dx on the DS's screen.
  struct Touch {
    std::int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0;
  };

  bool on = false;
  std::array<std::int8_t, 128> sprite_dx{};  // By sprite (attribute entry): how far across.
  std::array<Area, kMostAreas> areas{};
  unsigned area_count = 0;
  std::array<Touch, kMostTouches> touches{};
  unsigned touch_count = 0;

  // Where background `layer`'s pixel at x on `line` comes from: x itself,
  // or from x - dx (moved there), or kEmpty (moved away).
  static constexpr int kEmpty = INT_MIN;
  int source_x(unsigned layer, int line, int x) const;
  // Whether any area of background `layer` is on `line`.
  bool moves(unsigned layer, int line) const;
  bool moves_sprites() const;
  // A touch at x, y on the wide screen (DS pixels): where it is on the DS's
  // screen, as moved back.
  int touch_x(int x, int y) const;

  // For the graphics hardware (engine2d.frag reads them after the lines):
  // word 0, 1 << 31 if on and the number of areas; words 1 to 32, the
  // sprites' dx, a byte each; then three words an area: x0 and x1, y0 and
  // y1, dx and layers (16 bits each, x0 or y0 or dx the lower); from word
  // kMovedSpritesAt, four words: a bit for each sprite moved (so that the
  // sides' pass goes through those only).
  static constexpr std::size_t kWords = 64;
  static constexpr std::size_t kMovedSpritesAt = 57;
  void to_words(std::uint32_t* words) const;

  void clear() { *this = HudPlan{}; }
  void add_area(int x0, int y0, int x1, int y1, int dx, unsigned layers);
  void add_touch(int x0, int y0, int x1, int y1, int dx);
};

class WideHud {
 public:
  // How far an element moves: the width of a side of the wide screen, in
  // whole DS pixels (the 2D layers move by whole pixels; the 3D with them).
  // 0 switches it off.
  void set_shift(int columns) { shift_ = columns; }
  int shift() const { return shift_; }
  bool on() const { return shift_ > 0; }

  // The 3D part: the wide frame as the geometry engine finished it, made
  // into `out` as it is drawn: the HUD's flat polygons moved, or made
  // longer; the polygons kept for the HUD past the DS's view
  // (GeometryEngine::Polygon::beyond) used in a longer bar of commands,
  // else left out. Remembers which elements were found, for plan_2d().
  void apply_3d(const GeometryEngine::Frame& frame, GeometryEngine::Frame& out);

  // The 2D part, as a frame begins: what the main engine's picture moves,
  // from the elements found in the 3D and what is in the 2D layers.
  struct Screen2D {
    const std::uint8_t* sprites = nullptr;  // The main engine's sprite attribute memory (1 KiB).
    bool sprites_on = false;                // The main engine shows sprites.
    // The text layer: the background that holds the HUD's words and numbers
    // (background 1 over the 3D layer), or -1 if none.
    int text_layer = -1;
    // Whether background `layer` has a pixel in x0..x1, y0..y1 (not
    // including x1 and y1).
    std::function<bool(unsigned layer, int x0, int y0, int x1, int y1)> has_pixels;
  };
  const HudPlan& plan_2d(const Screen2D& screen);
  const HudPlan& plan() const { return plan_; }
  void clear_plan() { plan_.clear(); }

  // Which elements the last frames found, by name (for the log and tests).
  std::string found() const;

 private:
  // An element of the 3D found in the frame, for the 2D part.
  struct Found {
    bool on = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // Where it was (before it moved).
    float cut_left = 0, cut_right = 0;      // A bar: where its ends begin.
    float frame_y0 = 0;                     // A bar: the top of its frame (the B button can reach higher).
  };
  enum Element : unsigned { kCoins, kPrice, kItemInfo, kCommandRing, kHelpBar, kLogBand, kLabel, kElements };
  std::array<Found, kElements> found_{};
  bool flat_over_stats_ = false;  // A flat polygon where the party's numbers are (a speech bubble).
  int shift_ = 0;
  HudPlan plan_;
};

}  // namespace bd::nds
