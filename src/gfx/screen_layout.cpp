#include "gfx/screen_layout.h"

#include <algorithm>
#include <cmath>

namespace bd::gfx {

namespace {

struct Size {
  double x = 0, y = 0;
};

// A screen's width at `scale`: a wide one's as 256 * scale * 4 / 3, which
// is exact where it is whole.
double screen_width(bool wide, double scale) { return wide ? kScreenWidth * scale * 4 / 3 : kScreenWidth * scale; }
double top_width(const LayoutOptions& options, double scale) { return screen_width(options.wide_screen == 0, scale); }
double bottom_width(const LayoutOptions& options, double scale) { return screen_width(options.wide_screen == 1, scale); }

// The pair's size at `scale` (1: in DS pixels).
Size unscaled_size(const LayoutOptions& options, double scale = 1) {
  const double gap = std::max(options.gap, 0) * scale, height = kScreenHeight * scale;
  const double top = top_width(options, scale), bottom = bottom_width(options, scale);
  if (options.arrangement == Arrangement::BottomOnly) return {bottom, height};
  if (options.arrangement == Arrangement::Stacked) return {std::max(top, bottom), 2 * height + gap};
  return {top + bottom + gap, height};
}

}  // namespace

ScreenLayout lay_out(int width, int height, const LayoutOptions& options) {
  ScreenLayout layout;
  if (width <= 0 || height <= 0) return layout;
  const Size size = unscaled_size(options);
  const double fit = std::min(static_cast<double>(width) / size.x, static_cast<double>(height) / size.y);
  double scale = fit;
  if (options.scaling == Scaling::WholeNumbers && fit >= 1) scale = std::floor(fit);
  layout.scale = scale;
  layout.top_wide = options.wide_screen == 0 && options.arrangement != Arrangement::BottomOnly;
  layout.bottom_wide = options.wide_screen == 1;

  const Size scaled = unscaled_size(options, scale);
  const double h = kScreenHeight * scale;
  const double top_w = top_width(options, scale), bottom_w = bottom_width(options, scale);
  const double gap = std::max(options.gap, 0) * scale;
  const double left = (width - scaled.x) / 2, top = (height - scaled.y) / 2;
  if (options.arrangement == Arrangement::BottomOnly) {
    layout.top = Rect{left + bottom_w / 2, top + h / 2, 0, 0};
    layout.bottom = Rect{left, top, bottom_w, h};
    return layout;
  }
  const double first_w = options.bottom_first ? bottom_w : top_w, second_w = options.bottom_first ? top_w : bottom_w;
  Rect first, second;
  if (options.arrangement == Arrangement::Stacked) {
    // Each centred across the wider.
    const double across = scaled.x;
    first = Rect{left + (across - first_w) / 2, top, first_w, h};
    second = Rect{left + (across - second_w) / 2, top + h + gap, second_w, h};
  } else {
    first = Rect{left, top, first_w, h};
    second = Rect{left + first_w + gap, top, second_w, h};
  }
  layout.top = options.bottom_first ? second : first;
  layout.bottom = options.bottom_first ? first : second;
  return layout;
}

Point window_size(int scale, const LayoutOptions& options) {
  const Size size = unscaled_size(options, std::max(scale, 1));
  return {static_cast<int>(std::lround(size.x)), static_cast<int>(std::lround(size.y))};
}

std::optional<Point> touch_point(const ScreenLayout& layout, double x, double y) {
  Rect r = layout.bottom;
  if (layout.bottom_wide) {
    // The DS's screen: the columns in the middle.
    const double w = r.w * kScreenWidth / kWideWidth;
    r = Rect{r.x + (r.w - w) / 2, r.y, w, r.h};
  }
  if (r.w <= 0 || r.h <= 0) return std::nullopt;
  if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return std::nullopt;
  const int across = static_cast<int>((x - r.x) * kScreenWidth / r.w);
  const int down = static_cast<int>((y - r.y) * kScreenHeight / r.h);
  return Point{std::clamp(across, 0, kScreenWidth - 1), std::clamp(down, 0, kScreenHeight - 1)};
}

int prescale_factor(double scale, int largest_texture) {
  int factor = 1;
  if (scale > 1) {
    // A hair below a whole number counts as it: the layout's arithmetic is
    // in doubles.
    factor = static_cast<int>(std::ceil(scale - 1e-6));
  }
  const int most = largest_texture / kScreenWidth;
  if (factor > most) factor = most;
  return factor < 1 ? 1 : factor;
}

int enlarge_factor(ScreenFilter filter, double scale, int largest_texture, int made_at) {
  if (filter != ScreenFilter::SharpSmoothEdges || made_at < 1) return made_at;
  // The smallest whole multiple of the picture at or above the scale, so that
  // each of its pixels stays a block and the smoothing only shrinks.
  const int at_least = prescale_factor(scale, 1 << 30);
  int factor = (at_least + made_at - 1) / made_at * made_at;
  const int most = largest_texture / kScreenWidth / made_at * made_at;
  if (factor > most) factor = most;
  return factor < made_at ? made_at : factor;
}

}  // namespace bd::gfx
