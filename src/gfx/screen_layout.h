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
// Nothing here knows about a window system: sizes are in window pixels.
#pragma once

#include <optional>

namespace bd::gfx {

inline constexpr int kScreenWidth = 256, kScreenHeight = 192;

// BottomOnly: the touch screen alone; the top screen is not shown (its
// rectangle has no area), and the gap and which is first do not matter.
enum class Arrangement { Stacked, SideBySide, BottomOnly };
enum class Scaling { WholeNumbers, Fit };

struct LayoutOptions {
  Arrangement arrangement = Arrangement::Stacked;
  Scaling scaling = Scaling::WholeNumbers;
  int gap = 0;                // DS pixels between the screens; 0 or more.
  bool bottom_first = false;  // The touch screen above (or left of) the other.
};

struct Rect {
  double x = 0, y = 0, w = 0, h = 0;
};

struct ScreenLayout {
  Rect top, bottom;  // The console's top screen and its touch screen.
  double scale = 0;  // Window pixels per DS pixel.
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
  // another, and no blur. The screen is enlarged without smoothing to a
  // whole multiple of its size (prescale_factor), then smoothed (bilinear)
  // to the window: the look of melonDS's OpenGL display, which enlarges the
  // screens to a whole-number internal resolution and then scales them to
  // the window with linear filtering. At whole-number sizes it is the same
  // as Sharp.
  SharpSmoothEdges,
  Smooth,  // Bilinear from the screen's own pixels: soft throughout.
  Sharp,   // Nearest: each DS pixel a block, some a pixel wider at sizes that are not whole numbers.
};

// The whole multiple of the screen's size to enlarge it to before
// smoothing it to `scale` window pixels per DS pixel: the smallest that is
// at least the scale, so that the smoothing only ever shrinks, by less than
// half; 1 when the scale is below 1. No larger than a texture of
// `largest_texture` pixels across allows (and never below 1).
int prescale_factor(double scale, int largest_texture);

// The touch screen pixel (0-255 across, 0-191 down) under a point of the
// window, or nothing when the point is not on the touch screen.
std::optional<Point> touch_point(const ScreenLayout& layout, double x, double y);

}  // namespace bd::gfx
