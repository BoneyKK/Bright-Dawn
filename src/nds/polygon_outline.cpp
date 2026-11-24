#include "nds/polygon_outline.h"

#include <algorithm>

namespace bd::nds {

namespace {

using i64 = std::int64_t;

// Which side of the line through `owner`'s corners i and j each polygon is
// on: -1 or 1 if all its corners are on that side or on the line, at least
// one off it; 0 if neither.
int side_of(const PolygonOutline& p, i64 ax, i64 ay, i64 dx, i64 dy) {
  bool below = false, above = false;
  for (std::size_t k = 0; k < p.size; ++k) {
    // Twice the area of the triangle from the line's two corners to this
    // one: its sign is the side. At most 2^28 or so.
    const i64 s = dx * (p.y[k] - ay) - dy * (p.x[k] - ax);
    if (s < 0) below = true;
    if (s > 0) above = true;
  }
  return below == above ? 0 : below ? -1 : 1;
}

// Whether the line of `owner`'s edge from corner i to the next separates it
// from `other`.
bool edge_separates(const PolygonOutline& owner, std::size_t i, const PolygonOutline& other) {
  const std::size_t j = (i + 1) % owner.size;
  const i64 ax = owner.x[i], ay = owner.y[i];
  const i64 dx = owner.x[j] - ax, dy = owner.y[j] - ay;
  if (dx == 0 && dy == 0) return false;
  const int own = side_of(owner, ax, ay, dx, dy);
  return own != 0 && side_of(other, ax, ay, dx, dy) == -own;
}

}  // namespace

void PolygonOutline::bound() {
  left = top = 0x7FFFFFFF;
  right = bottom = -0x7FFFFFFF;
  for (std::size_t i = 0; i < size; ++i) {
    left = std::min(left, x[i]);
    right = std::max(right, x[i]);
    top = std::min(top, y[i]);
    bottom = std::max(bottom, y[i]);
  }
}

bool drawn_apart(const PolygonOutline& a, const PolygonOutline& b) {
  if (a.size < 3 || b.size < 3) return false;
  // Boxes that do not overlap, or only touch.
  if (a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top) return true;
  // A line along an edge of either, with each polygon's corners on its own
  // side: it separates them, whatever their shape. Between polygons that do
  // not bend inwards, such a line is there whenever any line separates them.
  for (std::size_t i = 0; i < a.size; ++i)
    if (edge_separates(a, i, b)) return true;
  for (std::size_t i = 0; i < b.size; ++i)
    if (edge_separates(b, i, a)) return true;
  return false;
}

}  // namespace bd::nds
