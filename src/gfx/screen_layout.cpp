#include "gfx/screen_layout.h"

#include <algorithm>
#include <cmath>

namespace bd::gfx {

namespace {

// The pair's size in DS pixels.
Point unscaled_size(const LayoutOptions& options) {
  const int gap = std::max(options.gap, 0);
  if (options.arrangement == Arrangement::Stacked) return {kScreenWidth, 2 * kScreenHeight + gap};
  return {2 * kScreenWidth + gap, kScreenHeight};
}

}  // namespace

ScreenLayout lay_out(int width, int height, const LayoutOptions& options) {
  ScreenLayout layout;
  if (width <= 0 || height <= 0) return layout;
  const Point size = unscaled_size(options);
  const double fit = std::min(static_cast<double>(width) / size.x, static_cast<double>(height) / size.y);
  double scale = fit;
  if (options.scaling == Scaling::WholeNumbers && fit >= 1) scale = std::floor(fit);
  layout.scale = scale;

  const double w = kScreenWidth * scale, h = kScreenHeight * scale;
  const double gap = std::max(options.gap, 0) * scale;
  const double left = (width - size.x * scale) / 2, top = (height - size.y * scale) / 2;
  const Rect first{left, top, w, h};
  const Rect second = options.arrangement == Arrangement::Stacked ? Rect{left, top + h + gap, w, h}
                                                                  : Rect{left + w + gap, top, w, h};
  layout.top = options.bottom_first ? second : first;
  layout.bottom = options.bottom_first ? first : second;
  return layout;
}

Point window_size(int scale, const LayoutOptions& options) {
  const Point size = unscaled_size(options);
  const int s = std::max(scale, 1);
  return {size.x * s, size.y * s};
}

std::optional<Point> touch_point(const ScreenLayout& layout, double x, double y) {
  const Rect& r = layout.bottom;
  if (r.w <= 0 || r.h <= 0) return std::nullopt;
  if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return std::nullopt;
  const int across = static_cast<int>((x - r.x) * kScreenWidth / r.w);
  const int down = static_cast<int>((y - r.y) * kScreenHeight / r.h);
  return Point{std::clamp(across, 0, kScreenWidth - 1), std::clamp(down, 0, kScreenHeight - 1)};
}

}  // namespace bd::gfx
