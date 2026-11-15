// The 3D engine making polygons from vertices: through the matrices, by the
// kind of vertex list, dropped when facing away, cut to the view volume, and
// placed on the screen. The expectations are worked out by hand from the
// hardware's arithmetic; W below is one unit (4096) and H half of it.
#include <cstdint>
#include <initializer_list>

#include "nds/arm9_io.h"
#include "nds/geometry_engine.h"
#include "nds/interrupts.h"
#include "test.h"

namespace {

using bd::nds::GeometryEngine;
using Vertex = GeometryEngine::Vertex;
using Polygon = GeometryEngine::Polygon;

constexpr int W = GeometryEngine::kOne;
constexpr int H = W / 2;
constexpr std::uint32_t kFront = GeometryEngine::kDrawFront;
constexpr std::uint32_t kBack = GeometryEngine::kDrawBack;
constexpr std::uint32_t kBoth = kFront | kBack;
constexpr std::uint32_t kWholeScreen = 0xBFFF0000;  // From (0, 0) to (255, 191).

struct Fixture {
  bd::nds::Interrupts irq;
  GeometryEngine g{irq};

  void send(unsigned command, std::initializer_list<std::uint32_t> parameters = {}) {
    if (parameters.size() == 0) g.write_command(command, 0);
    for (std::uint32_t p : parameters) g.write_command(command, p);
  }
  // A vertex, 16 bits a coordinate.
  void v(int x, int y, int z = 0) {
    send(GeometryEngine::kVertex16,
         {(static_cast<std::uint32_t>(x) & 0xFFFF) | (static_cast<std::uint32_t>(y) & 0xFFFF) << 16,
          static_cast<std::uint32_t>(z) & 0xFFFF});
  }
  void begin(unsigned kind, std::uint32_t attributes) {
    send(GeometryEngine::kPolygonAttributes, {attributes});
    send(GeometryEngine::kBeginVertices, {kind});
  }
  void texture_coordinates(int s, int t) {
    send(GeometryEngine::kTextureCoordinates,
         {(static_cast<std::uint32_t>(s) & 0xFFFF) | (static_cast<std::uint32_t>(t) & 0xFFFF) << 16});
  }
  // Ends the frame: "swap buffers" and the vertical blank it waits for.
  void swap(std::uint32_t parameter = 0) {
    send(GeometryEngine::kSwapBuffers, {parameter});
    g.vertical_blank();
  }
  std::size_t vertices() const { return g.building().vertices.size(); }
  std::size_t polygons() const { return g.building().polygons.size(); }
  const Vertex& vertex(std::size_t i) const { return g.building().vertices[i]; }
  const Polygon& polygon(std::size_t i) const { return g.building().polygons[i]; }
};

bool at(const Vertex& v, int x, int y) { return v.x == x && v.y == y; }
bool at(const Vertex& v, int x, int y, int z, int w) { return v.x == x && v.y == y && v.z == z && v.w == w; }

}  // namespace

TEST(polygons_a_triangle_is_made_and_placed_on_the_screen) {
  Fixture f;
  f.send(GeometryEngine::kViewport, {kWholeScreen});
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(-W, -W);
  f.v(W, -W);
  // Two vertices are no polygon yet.
  CHECK(f.g.counts() == 0x00020000);
  f.v(0, W);
  CHECK(f.g.counts() == 0x00030001);
  REQUIRE(f.vertices() == 3 && f.polygons() == 1);
  // Until the list ends the vertices are where the matrices put them.
  CHECK(at(f.vertex(0), -W, -W, 0, W) && at(f.vertex(1), W, -W, 0, W) && at(f.vertex(2), 0, W, 0, W));
  CHECK(f.polygon(0).first == 0 && f.polygon(0).size == 3);
  CHECK(!f.polygon(0).clockwise && !f.polygon(0).strip_order);
  CHECK(f.polygon(0).attributes == kBoth);

  // The frame stays until the vertical blank after "swap buffers".
  f.send(GeometryEngine::kSwapBuffers, {1});
  CHECK(f.g.counts() == 0x00030001 && f.g.finished().vertices.empty());
  f.g.vertical_blank();
  CHECK(f.g.counts() == 0);
  const GeometryEngine::Frame& frame = f.g.finished();
  REQUIRE(frame.vertices.size() == 3 && frame.polygons.size() == 1);
  CHECK(frame.swap_parameter == 1);
  // The left and bottom edges of the view are x 0 and y 192; the right edge
  // is x 256; y runs down. A depth in the middle of the view is 0x7FFE00.
  CHECK(at(frame.vertices[0], 0, 192, 0x7FFE00, W));
  CHECK(at(frame.vertices[1], 256, 192, 0x7FFE00, W));
  CHECK(at(frame.vertices[2], 128, 0, 0x7FFE00, W));
  const GeometryEngine::Statistics& s = f.g.statistics();
  CHECK(s.vertices == 3 && s.polygons == 1 && s.most_vertices == 3 && s.most_polygons == 1);
  CHECK(s.polygons_cut == 0 && s.polygons_facing_away == 0 && s.polygons_out_of_view == 0);

  // The next frame starts empty, and the one before stays until the next swap.
  f.swap(0);
  CHECK(f.g.finished().vertices.empty() && f.g.finished().polygons.empty() && f.g.finished().swap_parameter == 0);
}

TEST(polygons_vertices_go_through_position_and_projection_and_are_divided_by_w) {
  Fixture f;
  // A projection that makes every w two units, and a position matrix that
  // moves one unit to the right.
  f.send(GeometryEngine::kMatrixMode, {0});
  GeometryEngine::Matrix projection = GeometryEngine::kIdentity;
  projection[15] = 2 * W;
  for (std::int32_t value : projection) f.send(GeometryEngine::kMatrixLoad4x4, {static_cast<std::uint32_t>(value)});
  f.send(GeometryEngine::kMatrixMode, {1});
  f.send(GeometryEngine::kMatrixTranslate, {static_cast<std::uint32_t>(W), 0, 0});
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(-2 * W, -W);
  f.v(0, -W);
  // The matrix counts as it is when the vertex comes.
  f.send(GeometryEngine::kMatrixIdentity);
  f.v(W, W);
  REQUIRE(f.vertices() == 3 && f.polygons() == 1);
  CHECK(at(f.vertex(0), -W, -W, 0, 2 * W) && at(f.vertex(1), W, -W, 0, 2 * W) && at(f.vertex(2), W, W, 0, 2 * W));
  f.swap();
  // Half a view to either side of the middle: x 64 and 192, y 144 and 48.
  const GeometryEngine::Frame& frame = f.g.finished();
  REQUIRE(frame.vertices.size() == 3);
  CHECK(at(frame.vertices[0], 64, 144) && at(frame.vertices[1], 192, 144) && at(frame.vertices[2], 192, 48));
  CHECK(frame.vertices[0].z == 0x7FFE00 && frame.vertices[0].w == 2 * W);
}

TEST(polygons_depth_runs_from_the_near_side_to_the_far_side) {
  Fixture f;
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(0, 0, -W);
  f.v(H, 0, H);
  f.v(0, H, W);
  f.swap();
  const GeometryEngine::Frame& frame = f.g.finished();
  REQUIRE(frame.vertices.size() == 3 && frame.polygons.size() == 1);
  // (z / w) in 14 bits, plus 0x3FFF, times 512.
  CHECK(frame.vertices[0].z == -512);
  CHECK(frame.vertices[1].z == 0xBFFE00);
  CHECK(frame.vertices[2].z == 0xFFFE00);
}

TEST(polygons_facing_a_way_that_is_not_drawn_are_dropped) {
  Fixture f;
  // Counter-clockwise as seen is the front.
  f.begin(GeometryEngine::kTriangles, kBack);
  f.v(-W, -W);
  f.v(W, -W);
  f.v(0, W);
  CHECK(f.g.counts() == 0);
  CHECK(f.g.statistics().polygons_facing_away == 1);
  // The same the other way round is a back, stored as clockwise.
  f.v(-W, -W);
  f.v(0, W);
  f.v(W, -W);
  CHECK(f.g.counts() == 0x00030001);
  REQUIRE(f.polygons() == 1);
  CHECK(f.polygon(0).clockwise);

  // The attributes count as they were when the list began.
  f.send(GeometryEngine::kPolygonAttributes, {kFront});
  f.v(-W, -W);
  f.v(0, W);
  f.v(W, -W);
  CHECK(f.g.counts() == 0x00060002 && f.polygon(1).attributes == kBack);
  f.send(GeometryEngine::kBeginVertices, {GeometryEngine::kTriangles});
  f.v(-W, -W);
  f.v(0, W);
  f.v(W, -W);
  CHECK(f.g.counts() == 0x00060002);
  f.v(-W, -W);
  f.v(W, -W);
  f.v(0, W);
  CHECK(f.g.counts() == 0x00090003 && f.polygon(2).attributes == kFront && !f.polygon(2).clockwise);

  // With neither side drawn, nothing is.
  f.begin(GeometryEngine::kTriangles, 0);
  f.v(-W, -W);
  f.v(W, -W);
  f.v(0, W);
  f.v(-W, -W);
  f.v(0, W);
  f.v(W, -W);
  CHECK(f.g.counts() == 0x00090003);
  CHECK(f.g.statistics().polygons_facing_away == 4 && f.g.statistics().polygons == 3);
}

TEST(polygons_four_corners_and_vertices_left_over) {
  Fixture f;
  f.begin(GeometryEngine::kQuads, kBoth);
  f.v(-W, -W);
  f.v(W, -W);
  f.v(W, W);
  CHECK(f.g.counts() == 0x00030000);
  f.v(-W, W);
  CHECK(f.g.counts() == 0x00040001);
  REQUIRE(f.polygons() == 1);
  CHECK(f.polygon(0).first == 0 && f.polygon(0).size == 4 && !f.polygon(0).strip_order);
  // Two vertices of a polygon that is never finished are forgotten when the
  // next list begins,
  f.v(0, 0);
  f.v(H, 0);
  CHECK(f.g.counts() == 0x00060001);
  f.begin(GeometryEngine::kTriangles, kBoth);
  CHECK(f.g.counts() == 0x00040001);
  // and when the frame ends.
  f.v(0, 0);
  f.v(H, 0);
  CHECK(f.g.counts() == 0x00060001);
  f.swap();
  CHECK(f.g.finished().vertices.size() == 4 && f.g.finished().polygons.size() == 1);
  // A new frame's first vertices are not taken for the rest of that polygon.
  f.v(0, H);
  CHECK(f.g.counts() == 0x00010000);
}

TEST(polygons_a_strip_of_triangles_shares_vertices) {
  Fixture f;
  // Only fronts are drawn: every other triangle of a strip is sent the other
  // way round and still counts as a front.
  f.begin(GeometryEngine::kTriangleStrip, kFront);
  f.v(-W, -W);
  f.v(0, -W);
  f.v(-W, 0);
  CHECK(f.g.counts() == 0x00030001);
  f.v(0, 0);
  CHECK(f.g.counts() == 0x00040002);
  f.v(-W, W);
  CHECK(f.g.counts() == 0x00050003);
  REQUIRE(f.polygons() == 3);
  CHECK(f.polygon(0).first == 0 && f.polygon(1).first == 1 && f.polygon(2).first == 2);
  CHECK(f.polygon(0).size == 3 && f.polygon(1).size == 3 && f.polygon(2).size == 3);
  CHECK(!f.polygon(0).clockwise && f.polygon(1).clockwise && !f.polygon(2).clockwise);
}

TEST(polygons_a_triangle_dropped_breaks_its_strip) {
  Fixture f;
  f.begin(GeometryEngine::kTriangleStrip, kFront);
  f.v(-W, -W);
  f.v(0, -W);
  f.v(-W, 0);
  // The second triangle, folded back over the first, faces away.
  f.v(-W, -W);
  CHECK(f.g.statistics().polygons_facing_away == 1);
  // The first triangle keeps its three vertices; a new strip starts with
  // copies of the last two sent.
  CHECK(f.g.counts() == 0x00050001);
  REQUIRE(f.vertices() == 5);
  CHECK(at(f.vertex(2), -W, 0) && at(f.vertex(3), -W, 0) && at(f.vertex(4), -W, -W));
  // The third triangle is the new strip's first, and is turned the way it
  // would have been in the old one.
  f.v(0, 0);
  CHECK(f.g.counts() == 0x00060002);
  REQUIRE(f.polygons() == 2);
  CHECK(f.polygon(1).first == 3 && !f.polygon(1).clockwise);
  // The two vertices a broken strip went on with are forgotten if nothing
  // comes of them.
  f.begin(GeometryEngine::kTriangleStrip, kFront);
  f.v(-W, -W);
  f.v(0, -W);
  f.v(-W, 0);
  f.v(-W, -W);
  CHECK(f.g.counts() == 0x000B0003);
  f.begin(GeometryEngine::kTriangles, kFront);
  CHECK(f.g.counts() == 0x00090003);
}

TEST(polygons_a_strip_whose_first_triangle_is_dropped_goes_on) {
  Fixture f;
  f.begin(GeometryEngine::kTriangleStrip, kFront);
  f.v(-W, -W);
  f.v(-W, 0);
  f.v(0, -W);
  // A back. Its first vertex goes, the other two stay for the next triangle.
  CHECK(f.g.counts() == 0x00020000);
  CHECK(at(f.vertex(0), -W, 0) && at(f.vertex(1), 0, -W));
  f.v(-W, -W);
  // The strip's second triangle: sent clockwise, and so a front.
  CHECK(f.g.counts() == 0x00030001);
  REQUIRE(f.polygons() == 1);
  CHECK(f.polygon(0).first == 0 && f.polygon(0).clockwise);
  CHECK(f.g.statistics().polygons_facing_away == 1);
}

TEST(polygons_a_strip_of_four_cornered_polygons) {
  Fixture f;
  f.begin(GeometryEngine::kQuadStrip, kBoth);
  f.v(-W, -W);
  f.v(-W, 0);
  f.v(0, -W);
  CHECK(f.g.counts() == 0x00030000);
  f.v(0, 0);
  CHECK(f.g.counts() == 0x00040001);
  f.v(W, -W);
  CHECK(f.g.counts() == 0x00050001);
  f.v(W, 0);
  CHECK(f.g.counts() == 0x00060002);
  REQUIRE(f.polygons() == 2);
  // Each polygon is two vertices on from the last, and its corners are
  // stored as the strip sent them.
  CHECK(f.polygon(0).first == 0 && f.polygon(1).first == 2);
  CHECK(f.polygon(0).size == 4 && f.polygon(0).strip_order && f.polygon(1).strip_order);
  // Around the polygon the corners run 0, 1, 3, 2: here clockwise.
  CHECK(f.polygon(0).clockwise && f.polygon(1).clockwise);
  // One vertex too many is forgotten.
  f.v(W, W);
  CHECK(f.g.counts() == 0x00070002);
  f.begin(GeometryEngine::kQuadStrip, kFront);
  CHECK(f.g.counts() == 0x00060002);

  // The same strip with only fronts drawn: the first polygon goes and the
  // strip goes on from its last two vertices; so does the second.
  f.v(-W, -W);
  f.v(-W, 0);
  f.v(0, -W);
  f.v(0, 0);
  CHECK(f.g.counts() == 0x00080002);
  CHECK(at(f.vertex(6), 0, -W) && at(f.vertex(7), 0, 0));
  f.v(W, -W);
  f.v(W, 0);
  CHECK(f.g.counts() == 0x00080002);
  CHECK(at(f.vertex(6), W, -W) && at(f.vertex(7), W, 0));
  f.begin(GeometryEngine::kTriangles, kBoth);
  CHECK(f.g.counts() == 0x00060002);
  CHECK(f.g.statistics().polygons_facing_away == 2);
}

TEST(polygons_a_four_cornered_polygon_dropped_breaks_its_strip) {
  Fixture f;
  f.begin(GeometryEngine::kQuadStrip, kBack);
  f.v(-W, -W);
  f.v(-W, 0);
  f.v(0, -W);
  f.v(0, 0);
  CHECK(f.g.counts() == 0x00040001);
  // The second polygon is twisted so that it shows its front.
  f.v(W, -W);
  f.v(-W, -W);
  CHECK(f.g.statistics().polygons_facing_away == 1);
  // Its vertices stay: the first two belong to the polygon before, and a new
  // strip starts with the last two.
  CHECK(f.g.counts() == 0x00060001);
  f.v(W, 0);
  f.v(-W, 0);
  CHECK(f.g.counts() == 0x00080002);
  REQUIRE(f.polygons() == 2);
  CHECK(f.polygon(1).first == 4 && f.polygon(1).size == 4 && f.polygon(1).strip_order && f.polygon(1).clockwise);
}

TEST(polygons_are_cut_to_the_view) {
  Fixture f;
  f.begin(GeometryEngine::kTriangles, kBoth);
  // A triangle whose second corner is as far outside the right edge as its
  // first is inside. Black, bright red, black; texture coordinates too.
  f.send(GeometryEngine::kColour, {0});
  f.texture_coordinates(0, 0);
  f.v(0, 0);
  f.send(GeometryEngine::kColour, {0x001F});
  f.texture_coordinates(160, 0);
  f.v(2 * W, 0);
  f.send(GeometryEngine::kColour, {0});
  f.texture_coordinates(0, 320);
  f.v(0, W);
  // The corner outside is replaced by the two places where its edges cross
  // the right edge, and everything a vertex carries is taken in proportion.
  CHECK(f.g.counts() == 0x00040001);
  REQUIRE(f.vertices() == 4 && f.polygons() == 1);
  CHECK(f.polygon(0).first == 0 && f.polygon(0).size == 4);
  CHECK(at(f.vertex(0), 0, 0, 0, W) && f.vertex(0).colour == 0 && f.vertex(0).s == 0 && f.vertex(0).t == 0);
  CHECK(at(f.vertex(1), W, 0, 0, W) && f.vertex(1).colour == 31 && f.vertex(1).s == 80 && f.vertex(1).t == 0);
  CHECK(at(f.vertex(2), W, H, 0, W) && f.vertex(2).colour == 31 && f.vertex(2).s == 80 && f.vertex(2).t == 160);
  CHECK(at(f.vertex(3), 0, W, 0, W) && f.vertex(3).colour == 0 && f.vertex(3).s == 0 && f.vertex(3).t == 320);
  CHECK(f.g.statistics().polygons_cut == 1);

  // A triangle wholly outside is dropped.
  f.v(2 * W, 0);
  f.v(3 * W, 0);
  f.v(2 * W, W);
  CHECK(f.g.counts() == 0x00040001);
  CHECK(f.g.statistics().polygons_out_of_view == 1);

  f.swap();
  const GeometryEngine::Frame& frame = f.g.finished();
  REQUIRE(frame.vertices.size() == 4);
  CHECK(at(frame.vertices[0], 128, 96) && at(frame.vertices[1], 256, 96) && at(frame.vertices[2], 256, 48) &&
        at(frame.vertices[3], 128, 0));
}

TEST(polygons_a_triangle_cut_breaks_its_strip) {
  Fixture f;
  f.begin(GeometryEngine::kTriangleStrip, kBoth);
  f.v(0, 0);
  f.v(H, 0);
  f.v(0, W);
  CHECK(f.g.counts() == 0x00030001);
  f.v(2 * W, W);
  // The first triangle keeps its vertices. After them come the four corners
  // of the cut second triangle, and then copies of the last two vertices
  // sent, with which a new strip starts.
  CHECK(f.g.counts() == 0x00090002);
  REQUIRE(f.vertices() == 9 && f.polygons() == 2);
  CHECK(at(f.vertex(0), 0, 0) && at(f.vertex(1), H, 0) && at(f.vertex(2), 0, W));
  CHECK(at(f.vertex(3), W, 1365, 0, W) && at(f.vertex(4), H, 0) && at(f.vertex(5), 0, W) && at(f.vertex(6), W, W, 0, W));
  CHECK(at(f.vertex(7), 0, W) && at(f.vertex(8), 2 * W, W));
  CHECK(f.polygon(1).first == 3 && f.polygon(1).size == 4 && f.polygon(1).clockwise);
  // Nothing comes of the new strip.
  f.begin(GeometryEngine::kTriangles, kBoth);
  CHECK(f.g.counts() == 0x00070002);

  // When it is the strip's first triangle that is cut, its three vertices
  // are replaced.
  f.begin(GeometryEngine::kTriangleStrip, kBoth);
  f.v(0, 0);
  f.v(2 * W, 0);
  f.v(0, W);
  CHECK(f.g.counts() == 0x000D0003);
  CHECK(f.polygon(2).first == 7 && f.polygon(2).size == 4);
  CHECK(at(f.vertex(11), 2 * W, 0) && at(f.vertex(12), 0, W));
}

TEST(polygons_a_four_cornered_polygon_cut_breaks_its_strip) {
  Fixture f;
  f.begin(GeometryEngine::kQuadStrip, kBoth);
  f.v(0, 0);
  f.v(0, H);
  f.v(H, 0);
  f.v(H, H);
  CHECK(f.g.counts() == 0x00040001);
  f.v(2 * W, 0);
  f.v(2 * W, H);
  CHECK(f.g.counts() == 0x000A0002);
  REQUIRE(f.vertices() == 10 && f.polygons() == 2);
  // The cut polygon's corners are stored in the order they run around it.
  CHECK(f.polygon(0).strip_order && !f.polygon(1).strip_order);
  CHECK(f.polygon(1).first == 4 && f.polygon(1).size == 4);
  CHECK(at(f.vertex(4), W, 0, 0, W) && at(f.vertex(5), H, 0) && at(f.vertex(6), H, H) && at(f.vertex(7), W, H, 0, W));
  // The new strip starts with the last two vertices sent, in the order sent.
  CHECK(at(f.vertex(8), 2 * W, 0) && at(f.vertex(9), 2 * W, H));
}

TEST(polygons_crossing_the_far_side_are_dropped_unless_cutting_there_is_on) {
  Fixture f;
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(0, 0, 0);
  f.v(H, 0, 0);
  f.v(0, H, 2 * W);
  CHECK(f.g.counts() == 0);
  CHECK(f.g.statistics().polygons_past_far_side == 1 && f.g.statistics().polygons_out_of_view == 0);
  f.begin(GeometryEngine::kTriangles, kBoth | GeometryEngine::kCutAtFarSide);
  f.v(0, 0, 0);
  f.v(H, 0, 0);
  f.v(0, H, 2 * W);
  CHECK(f.g.counts() == 0x00040001);
  REQUIRE(f.vertices() == 4);
  // The corner behind the far side is replaced by two on it, half way along
  // its edges.
  CHECK(at(f.vertex(0), 0, H / 2, W, W) && at(f.vertex(1), 0, 0, 0, W));
  CHECK(at(f.vertex(2), H, 0, 0, W) && at(f.vertex(3), H / 2, H / 2, W, W));
  // The near side is always cut at.
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(0, 0, 0);
  f.v(H, 0, 0);
  f.v(0, H, -2 * W);
  CHECK(f.g.counts() == 0x00080002);
  CHECK(f.g.statistics().polygons_cut == 2);
}

TEST(polygons_a_frame_holds_2048_polygons_and_6144_vertices) {
  Fixture f;
  f.begin(GeometryEngine::kTriangles, kBoth);
  for (int i = 0; i < 2048; ++i) {
    f.v(0, 0);
    f.v(H, 0);
    f.v(0, H);
  }
  CHECK(f.g.counts() == 0x18000800);
  f.v(0, 0);
  f.v(H, 0);
  f.v(0, H);
  CHECK(f.g.counts() == 0x18000800);
  CHECK(f.g.statistics().vertices_without_room == 3 && f.g.statistics().polygons_without_room == 0);
  f.swap();
  CHECK(f.g.counts() == 0);
  CHECK(f.g.statistics().most_vertices == 6144 && f.g.statistics().most_polygons == 2048);

  // A strip makes 2048 triangles of 2050 vertices; the next vertex makes
  // none.
  f.begin(GeometryEngine::kTriangleStrip, kBoth);
  for (int i = 0; i < 2051; ++i) f.v((i & 1) * H, ((i >> 1) & 1) * H);
  CHECK(f.g.counts() == (2051u << 16 | 2048u));
  CHECK(f.g.statistics().polygons_without_room == 1);
  f.swap();

  // 1536 triangles cut to four corners each fill the vertices.
  f.begin(GeometryEngine::kTriangles, kBoth);
  for (int i = 0; i < 1536; ++i) {
    f.v(0, 0);
    f.v(2 * W, 0);
    f.v(0, W);
  }
  CHECK(f.g.counts() == (6144u << 16 | 1536u));
  f.v(0, 0);
  f.v(H, 0);
  f.v(0, H);
  CHECK(f.g.counts() == (6144u << 16 | 1536u));
  CHECK(f.g.statistics().vertices_without_room == 6);
}

TEST(polygons_texture_coordinates_as_sent_or_through_the_texture_matrix) {
  Fixture f;
  // The texture matrix: twice the first coordinate, and 3 and -1/2 added, in
  // the coordinates' own units (sixteenths of a texel).
  f.send(GeometryEngine::kMatrixMode, {3});
  f.send(GeometryEngine::kMatrixTranslate, {static_cast<std::uint32_t>(3 * W), static_cast<std::uint32_t>(-H), 0});
  f.send(GeometryEngine::kMatrixScale, {static_cast<std::uint32_t>(2 * W), static_cast<std::uint32_t>(W),
                                         static_cast<std::uint32_t>(W)});
  f.begin(GeometryEngine::kTriangles, kBoth);

  // As sent.
  f.send(GeometryEngine::kTextureParameters, {0});
  f.texture_coordinates(16, -32);
  f.v(0, 0);
  // Through the matrix when the coordinates are sent: 16 * 2 + 3, and
  // -32 - 1/2 rounded down.
  f.send(GeometryEngine::kTextureParameters, {1u << 30});
  f.texture_coordinates(16, -32);
  f.v(H, 0);
  // From the vertex, when it is sent: its position through the matrix's
  // 3 by 3, a unit of position counting as a sixteenth of a texel, added to
  // the coordinates sent. 2 + 16, and -1/2 rounded down - 32.
  f.send(GeometryEngine::kTextureParameters, {3u << 30});
  f.v(W, -H);
  REQUIRE(f.vertices() == 3 && f.polygons() == 1);
  CHECK(f.vertex(0).s == 16 && f.vertex(0).t == -32);
  CHECK(f.vertex(1).s == 35 && f.vertex(1).t == -33);
  CHECK(f.vertex(2).s == 18 && f.vertex(2).t == -33);
  // The polygon carries the texture words in force when it was made.
  f.send(GeometryEngine::kPaletteBase, {0xFFFFFFFF});
  f.send(GeometryEngine::kTextureParameters, {0x12345678});
  f.v(0, 0);
  f.v(H, 0);
  f.v(0, H);
  REQUIRE(f.polygons() == 2);
  CHECK(f.polygon(0).texture == 3u << 30 && f.polygon(0).palette == 0);
  CHECK(f.polygon(1).texture == 0x12345678 && f.polygon(1).palette == 0x1FFF);
}

TEST(polygons_vertex_colours) {
  Fixture f;
  f.begin(GeometryEngine::kTriangles, kBoth);
  // Five bits a part are made six: doubled, and one more unless zero.
  f.send(GeometryEngine::kColour, {0x7FFF});
  f.v(0, 0);
  f.send(GeometryEngine::kColour, {0x0001 | 16u << 5});
  f.v(H, 0);
  // The material's diffuse colour becomes the vertex colour when bit 15 says
  // so, and not otherwise.
  f.send(GeometryEngine::kDiffuseAmbient, {0x8000u | 0x1Fu << 10});
  f.send(GeometryEngine::kDiffuseAmbient, {0x03E0});
  f.v(0, H);
  REQUIRE(f.vertices() == 3);
  CHECK(f.vertex(0).colour == 0x3FFFF);
  CHECK(f.vertex(1).colour == (3u | 33u << 6));
  CHECK(f.vertex(2).colour == 63u << 12);
}

TEST(polygons_the_viewport_counts_from_the_next_vertex_list) {
  Fixture f;
  f.send(GeometryEngine::kViewport, {kWholeScreen});
  f.begin(GeometryEngine::kTriangles, kBoth);
  f.v(-W, W);
  f.v(-W, -W);
  f.v(W, -W);
  // The middle half of the screen: from (64, 48) to (191, 143), with y
  // counted from the bottom.
  f.send(GeometryEngine::kViewport, {64u | 48u << 8 | 191u << 16 | 143u << 24});
  f.v(-W, W);
  f.v(-W, -W);
  f.v(W, -W);
  f.send(GeometryEngine::kBeginVertices, {GeometryEngine::kTriangles});
  f.v(-W, W);
  f.v(-W, -W);
  f.v(W, -W);
  f.swap();
  const GeometryEngine::Frame& frame = f.g.finished();
  REQUIRE(frame.vertices.size() == 9);
  for (std::size_t i = 0; i < 6; i += 3)
    CHECK(at(frame.vertices[i], 0, 0) && at(frame.vertices[i + 1], 0, 192) && at(frame.vertices[i + 2], 256, 192));
  CHECK(at(frame.vertices[6], 64, 48) && at(frame.vertices[7], 64, 144) && at(frame.vertices[8], 192, 144));
}

TEST(polygons_the_count_register) {
  Fixture f;
  bd::nds::Arm9Io io;
  f.g.attach(io);
  CHECK(io.read(GeometryEngine::kCounts, 4) == 0);
  // Polygon attributes, begin, and three vertices of 10 bits a coordinate,
  // through the commands' own ports.
  io.write(0x040004A4, 4, kBoth);
  io.write(0x04000500, 4, GeometryEngine::kTriangles);
  io.write(0x04000490, 4, 0);
  io.write(0x04000490, 4, 0x020);
  io.write(0x04000490, 4, 0x020u << 10);
  CHECK(io.read(GeometryEngine::kCounts, 4) == 0x00030001);
  CHECK(io.read(GeometryEngine::kCounts, 2) == 1 && io.read(GeometryEngine::kCounts + 2, 2) == 3);
  // It cannot be written.
  io.write(GeometryEngine::kCounts, 4, 0);
  CHECK(io.read(GeometryEngine::kCounts, 4) == 0x00030001);
}
