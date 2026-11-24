// Whether two polygons of a 3D scene can be drawn in the same draw call of
// the graphics hardware without a change to any pixel.
//
// A translucent polygon whose texture has pixels of full alpha is drawn in
// two steps (HardwareScene::Kind::Solid, then Translucent or Overlay), and
// the DS draws each polygon's pixels before the next polygon's. Polygons
// that never draw the same pixel can have their first steps drawn
// together, then their second steps: on different pixels the order does not
// matter.
//
// Two polygons never draw the same pixel when a line separates them: each
// is on its own side of it, or on it. Their insides do not overlap, and
// they can touch: at a corner, or along an edge, as the polygons of a mesh
// do. Graphics hardware draws a pixel whose centre is on an edge for one
// triangle only: for the one that a point a very little to the right and
// below the centre is inside of (the "top left" rule of Direct3D, which
// Vulkan devices follow). So no pixel on such a line is drawn by both. The
// corners are on a grid of sixteenths of a DS pixel, which the hardware
// keeps exactly: it places corners on a grid as fine or finer.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bd::nds {

struct PolygonOutline {
  std::size_t size = 0;  // 3 to 10 corners, in the order around the polygon.
  // In sixteenths of a DS pixel, as the polygon is placed on the screen.
  std::array<std::int32_t, 10> x{}, y{};
  std::int32_t left = 0, top = 0, right = 0, bottom = 0;

  // Sets left, top, right and bottom from the corners.
  void bound();
};

bool drawn_apart(const PolygonOutline& a, const PolygonOutline& b);

}  // namespace bd::nds
