// Where the DS's two screens go in a window, and where a point of the window
// falls on the touch screen.
//
// The screens are 256 by 192 pixels each. They are shown one above the other
// (as on the console) or side by side, with an optional gap between them in
// DS pixels, which is scaled with the screens, or the touch screen alone.
// Either screen can be put first. What is shown is centred in the window
// and scaled:
// - by whole numbers (every DS pixel the same size on screen), the largest
//   that fits; when not even one fits, as by Fit;
// - or to fill as much of the window as the screens' shape allows (Fit).
//
// With the graphics hardware's widescreen (app/hardware_3d.h), one screen,
// the one with the 3D scene, is a third wider (16:9: kWideWidth DS pixels
// across), its 256 columns in the middle; the other stays 4:3, and in a
// stack it is centred over or under the wide one.
//
// Nothing here knows about a window system: sizes are in window pixels.
#pragma once

#include <optional>

namespace bd::gfx {

inline constexpr int kScreenWidth = 256, kScreenHeight = 192;
// A wide screen's width in DS pixels: 16:9 at the DS's 192 lines.
inline constexpr double kWideWidth = kScreenWidth * 4.0 / 3.0;

// BottomOnly: the touch screen alone; the top screen is not shown (its
// rectangle has no area), and the gap and which is first do not matter.
enum class Arrangement { Stacked, SideBySide, BottomOnly };
enum class Scaling { WholeNumbers, Fit };

struct LayoutOptions {
  Arrangement arrangement = Arrangement::Stacked;
  Scaling scaling = Scaling::WholeNumbers;
  int gap = 0;                // DS pixels between the screens; 0 or more.
  bool bottom_first = false;  // The touch screen above (or left of) the other.
  int wide_screen = -1;       // The screen shown wide: 0 the top, 1 the touch screen, -1 neither.
};

struct Rect {
  double x = 0, y = 0, w = 0, h = 0;
};

struct ScreenLayout {
  Rect top, bottom;  // The console's top screen and its touch screen.
  double scale = 0;  // Window pixels per DS pixel.
  // Whether each is wide (kWideWidth across, the DS's screen in the middle).
  bool top_wide = false, bottom_wide = false;
};

struct Point {
  int x = 0, y = 0;
};

// The screens' places in a window of `width` by `height` pixels. A window
// with no area gets screens with no area.
ScreenLayout lay_out(int width, int height, const LayoutOptions& options);

// The window size that shows the screens at `scale` (1 or more) with no
// border.
Point window_size(int scale, const LayoutOptions& options);

// How a screen's pixels are scaled to the window.
enum class ScreenFilter {
  // Each DS pixel a sharp block, with the boundary between two blocks mixed
  // over one window pixel where it falls inside one: no block wider than
  // another, and no blur. The screen's picture (the console's, or the
  // graphics hardware's at N times the resolution) is enlarged without
  // smoothing to a whole multiple of itself (enlarge_factor), then smoothed
  // (bilinear) to the window. At whole-number sizes it is the same as Sharp.
  SharpSmoothEdges,
  Smooth,  // Bilinear from the screen's own pixels: soft throughout.
  Sharp,   // Nearest: each DS pixel a block, some a pixel wider at sizes that are not whole numbers
           // (as melonDS shows the screens with its filter off).
};

// The whole multiple of the screen's size to enlarge it to before
// smoothing it to `scale` window pixels per DS pixel: the smallest that is
// at least the scale, so that the smoothing only ever shrinks, by less than
// half; 1 when the scale is below 1. No larger than a texture of
// `largest_texture` pixels across allows (and never below 1).
int prescale_factor(double scale, int largest_texture);

// The pixels per DS pixel a screen's picture, made at `made_at` pixels per
// DS pixel (1 for the console's own picture, N for the graphics hardware's
// at N times the resolution), is enlarged to without smoothing before the
// window shows it at `scale` window pixels per DS pixel with `filter`
// (smoothed but for Sharp). `made_at` when it is not enlarged.
int enlarge_factor(ScreenFilter filter, double scale, int largest_texture, int made_at);

// The touch screen pixel (0-255 across, 0-191 down) under a point of the
// window, or nothing when the point is not on the touch screen. On a wide
// touch screen, only its 256 columns in the middle are the DS's.
std::optional<Point> touch_point(const ScreenLayout& layout, double x, double y);

}  // namespace bd::gfx
