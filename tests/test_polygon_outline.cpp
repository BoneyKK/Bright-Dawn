// When two polygons of a 3D scene share no pixel (nds/polygon_outline.h).
#include <initializer_list>
#include <utility>

#include "nds/polygon_outline.h"
#include "test.h"

namespace {

using bd::nds::PolygonOutline;

// Corners in sixteenths of a pixel.
PolygonOutline outline(std::initializer_list<std::pair<int, int>> corners) {
  PolygonOutline o;
  for (const auto& [x, y] : corners) {
    o.x[o.size] = x;
    o.y[o.size] = y;
    ++o.size;
  }
  o.bound();
  return o;
}

bool apart(const PolygonOutline& a, const PolygonOutline& b) {
  // The same either way round.
  const bool one = bd::nds::drawn_apart(a, b), other = bd::nds::drawn_apart(b, a);
  CHECK(one == other);
  return one;
}

const PolygonOutline kSquare = outline({{0, 0}, {0, 128}, {128, 128}, {128, 0}});

}  // namespace

TEST(outline_polygons_apart_or_touching_share_no_pixel) {
  CHECK(apart(kSquare, outline({{200, 0}, {200, 128}, {300, 128}})));
  // An edge shared, as in a mesh; a part of one; a corner.
  CHECK(apart(kSquare, outline({{128, 0}, {128, 128}, {256, 128}, {256, 0}})));
  CHECK(apart(kSquare, outline({{128, 32}, {128, 64}, {256, 64}})));
  CHECK(apart(kSquare, outline({{128, 128}, {128, 256}, {256, 256}, {256, 128}})));
  // Across a slanted edge, which is no side of a box.
  const PolygonOutline left = outline({{0, 0}, {0, 128}, {100, 128}, {30, 0}});
  CHECK(apart(left, outline({{30, 0}, {100, 128}, {200, 128}, {200, 0}})));
  CHECK(apart(left, outline({{31, 0}, {101, 128}, {200, 128}, {200, 0}})));
}

TEST(outline_overlapping_polygons_share_pixels) {
  CHECK(!apart(kSquare, outline({{64, 64}, {64, 192}, {192, 192}, {192, 64}})));
  CHECK(!apart(kSquare, kSquare));
  // Over the slanted edge by a sixteenth.
  const PolygonOutline left = outline({{0, 0}, {0, 128}, {100, 128}, {30, 0}});
  CHECK(!apart(left, outline({{29, 0}, {100, 128}, {200, 128}, {200, 0}})));
  // One inside the other.
  CHECK(!apart(kSquare, outline({{32, 32}, {32, 64}, {64, 64}})));
}

TEST(outline_needs_three_corners) {
  CHECK(!apart(kSquare, outline({{200, 0}, {300, 0}})));
}
