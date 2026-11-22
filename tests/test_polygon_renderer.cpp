// Drawing a 3D frame's polygons. The frames are made by hand, with corners
// already on the screen, and the expectations worked out from the hardware's
// arithmetic.
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>

#include "nds/arm9_io.h"
#include "nds/geometry_engine.h"
#include "nds/polygon_renderer.h"
#include "nds/scene_dump.h"
#include "nds/video_memory.h"
#include "temp_dir.h"
#include "test.h"

namespace {

using bd::nds::GeometryEngine;
using bd::nds::PolygonRenderer;
using bd::nds::VideoMemory;
using Vertex = GeometryEngine::Vertex;
using u32 = std::uint32_t;

// Colours as drawn: six bits a part.
constexpr u32 kRed = 63, kGreen = 63u << 6, kBlue = 63u << 12, kWhite = 0x3FFFF, kBlack = 0;
constexpr u32 rgb(u32 r, u32 g, u32 b) { return r | g << 6 | b << 12; }
constexpr u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 6 | b << 12 | a << 18; }
constexpr u32 kSolid = 63u << 18;

// Polygon attributes.
constexpr u32 alpha(u32 a) { return a << 16; }
constexpr u32 number(u32 n) { return n << 24; }
constexpr u32 mode(u32 m) { return m << 4; }
constexpr u32 kOpaque = alpha(31);
constexpr u32 kFogged = 1u << 15, kDepthEqual = 1u << 14, kNewDepth = 1u << 11;

constexpr int kMiddle = 0x400000;  // A depth.
constexpr int kOne = 0x1000;       // A w.

struct Fixture {
  bd::nds::Arm9Io io;
  PolygonRenderer r;
  VideoMemory video;
  GeometryEngine::Frame frame;

  Fixture() {
    r.attach(io);
    io.write(PolygonRenderer::kClearDepth, 2, 0x7FFF);
  }
  void control(u32 value) { io.write(PolygonRenderer::kControl, 2, value); }

  static Vertex vertex(int x, int y, u32 colour = kWhite, int z = kMiddle, int w = kOne, int s = 0, int t = 0) {
    Vertex v;
    v.x = x, v.y = y, v.z = z, v.w = w;
    v.colour = colour;
    v.s = static_cast<std::int16_t>(s);
    v.t = static_cast<std::int16_t>(t);
    return v;
  }
  // A polygon with these corners. Which way round it runs is worked out as
  // the 3D engine's front half would.
  void polygon(std::initializer_list<Vertex> corners, u32 attributes, u32 texture = 0, u32 palette = 0) {
    GeometryEngine::Polygon p;
    p.first = static_cast<std::uint16_t>(frame.vertices.size());
    p.size = static_cast<std::uint8_t>(corners.size());
    p.attributes = attributes;
    p.texture = texture;
    p.palette = palette;
    const Vertex* c = corners.begin();
    p.clockwise = (c[1].x - c[0].x) * (c[2].y - c[0].y) - (c[1].y - c[0].y) * (c[2].x - c[0].x) > 0;
    frame.vertices.insert(frame.vertices.end(), corners.begin(), corners.end());
    frame.polygons.push_back(p);
  }
  // Pixels x0 to x1 - 1 of lines y0 to y1 - 1, in one colour at one depth.
  void rectangle(int x0, int y0, int x1, int y1, u32 colour, u32 attributes, int z = kMiddle, int w = kOne) {
    polygon({vertex(x0, y0, colour, z, w), vertex(x0, y1, colour, z, w), vertex(x1, y1, colour, z, w),
             vertex(x1, y0, colour, z, w)},
            attributes);
  }
  void clear() {
    frame.vertices.clear();
    frame.polygons.clear();
  }
  void draw() { r.draw(frame, video); }
  u32 at(int x, int y) const { return r.image()[static_cast<std::size_t>(y * PolygonRenderer::kWidth + x)]; }
  bool drawn(int x, int y) const { return (at(x, y) >> 18) != 0; }
  int drawn_on_line(int y) const {
    int count = 0;
    for (int x = 0; x < PolygonRenderer::kWidth; ++x) count += drawn(x, y) ? 1 : 0;
    return count;
  }
  int drawn_in_all() const {
    int count = 0;
    for (int y = 0; y < PolygonRenderer::kHeight; ++y) count += drawn_on_line(y);
    return count;
  }

  // Video memory for textures: bank A as the first texture slot, B as the
  // second, E as the palettes. Filled as plain memory, then handed over.
  void fill_textures() {
    video.set_control(VideoMemory::kA, VideoMemory::kEnable);
    video.set_control(VideoMemory::kB, VideoMemory::kEnable);
    video.set_control(VideoMemory::kE, VideoMemory::kEnable);
  }
  void texture_byte(u32 at_address, u32 value) { video.write(0x06800000 + at_address, 1, value); }
  void texture_word(u32 at_address, u32 value) { video.write(0x06800000 + at_address, 2, value); }
  void second_slot_word(u32 at_address, u32 value) { video.write(0x06820000 + at_address, 2, value); }
  void palette_word(u32 at_address, u32 value) { video.write(0x06880000 + at_address, 2, value); }
  void hand_over_textures() {
    video.set_control(VideoMemory::kA, VideoMemory::kEnable | 3);
    video.set_control(VideoMemory::kB, VideoMemory::kEnable | 3 | 8);
    video.set_control(VideoMemory::kE, VideoMemory::kEnable | 3);
  }
  // A square of `size` pixels at the top left with texture coordinates that
  // put texel (x, y) on pixel (x, y).
  void textured_square(int size, u32 texture, u32 attributes = kOpaque, u32 palette = 0, u32 colour = kWhite) {
    polygon({vertex(0, 0, colour, kMiddle, kOne, 0, 0), vertex(0, size, colour, kMiddle, kOne, 0, size * 16),
             vertex(size, size, colour, kMiddle, kOne, size * 16, size * 16),
             vertex(size, 0, colour, kMiddle, kOne, size * 16, 0)},
            attributes, texture, palette);
  }
};

// Texture parameter words.
constexpr u32 format(u32 f) { return f << 26; }
constexpr u32 kRepeatS = 1u << 16, kFlipS = 1u << 18, kTransparent0 = 1u << 29;

}  // namespace

TEST(renderer_registers) {
  Fixture f;
  // The control register can be read; of its bits, 0 to 11 and 14 are kept.
  f.io.write(PolygonRenderer::kControl, 2, 0xFFFF);
  CHECK(f.io.read(PolygonRenderer::kControl, 2) == 0x4FFF);
  f.control(0);
  // The overflow flag is set by the front half and cleared by writing a one.
  f.r.ram_overflowed();
  CHECK(f.io.read(PolygonRenderer::kControl, 2) == PolygonRenderer::kRamOverflow);
  f.control(PolygonRenderer::kFog);
  CHECK(f.io.read(PolygonRenderer::kControl, 2) == (PolygonRenderer::kRamOverflow | PolygonRenderer::kFog));
  f.control(PolygonRenderer::kRamOverflow | PolygonRenderer::kFog);
  CHECK(f.io.read(PolygonRenderer::kControl, 2) == PolygonRenderer::kFog);
  // The rest are written only, and keep the bits that exist.
  f.io.write(PolygonRenderer::kClearColour, 4, 0xFFFFFFFF);
  f.io.write(PolygonRenderer::kClearDepth, 2, 0xFFFF);
  f.io.write(PolygonRenderer::kFogColour, 4, 0xFFFFFFFF);
  f.io.write(PolygonRenderer::kFogOffset, 2, 0xFFFF);
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, 0xFFFF);
  CHECK(f.r.clear_colour() == 0x3F1FFFFF && f.r.clear_depth() == 0x7FFF);
  CHECK(f.r.fog_colour() == 0x001F7FFF && f.r.fog_offset() == 0x7FFF && f.r.alpha_test_reference() == 0x1F);
  CHECK(f.io.read(PolygonRenderer::kClearColour, 4) == 0 && f.io.read(PolygonRenderer::kFogTable, 1) == 0);
  f.io.write(PolygonRenderer::kEdgeColours + 14, 2, 0x1234);
  f.io.write(PolygonRenderer::kFogTable + 31, 1, 0x7F);
  f.io.write(PolygonRenderer::kToonTable + 62, 2, 0x4321);
  CHECK(f.io.unmodelled().empty());
}

TEST(renderer_an_empty_frame_is_the_clear_colour) {
  Fixture f;
  f.draw();
  // Nothing set: every pixel is black with alpha 0.
  CHECK(f.drawn_in_all() == 0 && f.at(0, 0) == 0 && f.at(255, 191) == 0);
  // Five bits a part made six; the alpha is the clear colour's own.
  f.io.write(PolygonRenderer::kClearColour, 4, 0x001F | 16u << 5 | 31u << 16);
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 33, 0, 63) && f.at(255, 191) == rgba(63, 33, 0, 63));
  CHECK(f.drawn_in_all() == 256 * 192);
  f.io.write(PolygonRenderer::kClearColour, 4, 0x7FFF | 8u << 16);
  f.draw();
  CHECK(f.at(100, 100) == rgba(63, 63, 63, 17));
}

TEST(renderer_a_polygon_covers_from_its_left_edge_to_before_its_right_and_bottom) {
  Fixture f;
  f.rectangle(10, 10, 20, 20, kRed, kOpaque);
  f.draw();
  CHECK(f.drawn_in_all() == 100);
  for (int y = 10; y < 20; ++y) CHECK(f.drawn_on_line(y) == 10);
  CHECK(f.at(10, 10) == (kRed | kSolid) && f.at(19, 19) == (kRed | kSolid));
  CHECK(!f.drawn(9, 10) && !f.drawn(20, 10) && !f.drawn(10, 9) && !f.drawn(10, 20));
  // The corners can come in any order, and either way round.
  f.clear();
  f.polygon({Fixture::vertex(20, 20), Fixture::vertex(20, 10), Fixture::vertex(10, 10), Fixture::vertex(10, 20)}, kOpaque);
  f.draw();
  CHECK(f.drawn_in_all() == 100 && f.drawn(10, 10) && f.drawn(19, 19));
  f.clear();
  f.polygon({Fixture::vertex(10, 10), Fixture::vertex(20, 10), Fixture::vertex(20, 20), Fixture::vertex(10, 20)}, kOpaque);
  f.draw();
  CHECK(f.drawn_in_all() == 100 && f.drawn(10, 10) && f.drawn(19, 19));
}

TEST(renderer_slanted_edges) {
  Fixture f;
  // A right edge that runs four pixels across for one down covers four
  // pixels a line.
  f.polygon({Fixture::vertex(0, 0), Fixture::vertex(0, 4), Fixture::vertex(16, 4)}, kOpaque);
  f.draw();
  for (int y = 0; y < 4; ++y) CHECK(f.drawn_on_line(y) == 4 * y + 4);
  CHECK(f.drawn(3, 0) && !f.drawn(4, 0) && f.drawn(15, 3) && !f.drawn(16, 3));
  CHECK(f.drawn_on_line(4) == 0);

  // A right edge at 45 degrees is on pixel x = y, and its pixels are left to
  // a polygon beyond it, except on the top line.
  f.clear();
  f.polygon({Fixture::vertex(0, 0, kRed), Fixture::vertex(0, 8, kRed), Fixture::vertex(8, 8, kRed)}, kOpaque);
  f.draw();
  CHECK(f.drawn_on_line(0) == 1 && f.drawn(0, 0));
  for (int y = 1; y < 8; ++y) CHECK(f.drawn_on_line(y) == y);
  CHECK(f.drawn(2, 3) && !f.drawn(3, 3));
  // The polygon beyond it, with that edge as its left edge, draws them: the
  // two make a square with no gap.
  f.polygon({Fixture::vertex(0, 0, kBlue), Fixture::vertex(8, 8, kBlue), Fixture::vertex(8, 0, kBlue)}, kOpaque);
  f.draw();
  CHECK(f.drawn_in_all() == 64);
  for (int y = 0; y < 8; ++y) CHECK(f.drawn_on_line(y) == 8);
  CHECK(f.at(2, 3) == (kRed | kSolid) && f.at(3, 3) == (kBlue | kSolid) && f.at(7, 7) == (kBlue | kSolid));
}

TEST(renderer_what_is_nearer_is_drawn) {
  Fixture f;
  // The nearer of two, whichever is sent first.
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x400000);
  f.rectangle(4, 0, 12, 8, kBlue, kOpaque, 0x200000);
  f.rectangle(8, 0, 16, 8, kGreen, kOpaque, 0x300000);
  f.draw();
  CHECK(f.at(0, 0) == (kRed | kSolid) && f.at(4, 0) == (kBlue | kSolid) && f.at(11, 0) == (kBlue | kSolid));
  CHECK(f.at(12, 0) == (kGreen | kSolid));
  // At the same depth the first stays,
  f.clear();
  f.rectangle(0, 0, 8, 8, kRed, kOpaque);
  f.rectangle(0, 0, 8, 8, kBlue, kOpaque);
  f.draw();
  CHECK(f.at(3, 3) == (kRed | kSolid));
  // unless the second asks to be drawn at equal depth; that allows 0x200
  // either way and no more.
  f.clear();
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x400000);
  f.rectangle(0, 0, 4, 8, kBlue, kOpaque | kDepthEqual, 0x400200);
  f.rectangle(4, 0, 8, 4, kBlue, kOpaque | kDepthEqual, 0x3FFE00);
  f.rectangle(4, 4, 8, 8, kBlue, kOpaque | kDepthEqual, 0x400201);
  f.draw();
  CHECK(f.at(0, 0) == (kBlue | kSolid) && f.at(4, 0) == (kBlue | kSolid) && f.at(4, 4) == (kRed | kSolid));
  // Nothing is drawn behind the clear depth: 0x1000 there is 0x200000.
  f.clear();
  f.io.write(PolygonRenderer::kClearDepth, 2, 0x1000);
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x200000);
  f.rectangle(8, 0, 16, 8, kRed, kOpaque, 0x1FFFFF);
  f.draw();
  CHECK(!f.drawn(0, 0) && f.drawn(8, 0));
}

TEST(renderer_depth_can_be_w) {
  Fixture f;
  f.frame.depth_is_w = true;
  // The smaller w is nearer, whatever z says. A w too large for 16 bits is
  // cut down for the arithmetic and a small one widened, and the depth is
  // w again.
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x100000, 0x100000);
  f.rectangle(0, 0, 8, 8, kBlue, kOpaque, 0x700000, 0x2000);
  f.rectangle(4, 0, 8, 8, kGreen, kOpaque, 0x7FFFFF, 0x10);
  f.rectangle(0, 4, 8, 8, kWhite, kOpaque, 0x000000, 0x4000);
  f.draw();
  CHECK(f.at(0, 0) == (kBlue | kSolid) && f.at(4, 0) == (kGreen | kSolid));
  CHECK(f.at(0, 4) == (kBlue | kSolid) && f.at(4, 4) == (kGreen | kSolid));
}

TEST(renderer_colours_run_between_the_corners) {
  Fixture f;
  // Black on the left, red on the right, 16 pixels apart: 504 * x / 16, in
  // eighths of a step.
  f.polygon({Fixture::vertex(0, 0, kBlack), Fixture::vertex(0, 4, kBlack), Fixture::vertex(16, 4, kRed),
             Fixture::vertex(16, 0, kRed)},
            kOpaque);
  f.draw();
  CHECK(f.at(0, 0) == kSolid && f.at(1, 0) == (3 | kSolid) && f.at(8, 0) == (31 | kSolid) && f.at(15, 0) == (59 | kSolid));
  CHECK(f.at(8, 3) == (31 | kSolid));
  // Black at the top, green at the bottom, four lines apart.
  f.clear();
  f.polygon({Fixture::vertex(0, 0, kBlack), Fixture::vertex(0, 4, kGreen), Fixture::vertex(16, 4, kGreen),
             Fixture::vertex(16, 0, kBlack)},
            kOpaque);
  f.draw();
  CHECK(f.at(0, 0) == kSolid && f.at(5, 1) == (15u << 6 | kSolid) && f.at(5, 2) == (31u << 6 | kSolid) &&
        f.at(15, 3) == (47u << 6 | kSolid));
}

TEST(renderer_colours_run_with_perspective) {
  Fixture f;
  // The right side is four times as far away as the left, so the far half
  // takes up less of the span: at the middle pixel the colour is only 51
  // 256ths of the way, and three quarters across 109.
  f.polygon({Fixture::vertex(0, 0, kBlack, kMiddle, 0x1000), Fixture::vertex(0, 4, kBlack, kMiddle, 0x1000),
             Fixture::vertex(16, 4, kRed, kMiddle, 0x4000), Fixture::vertex(16, 0, kRed, kMiddle, 0x4000)},
            kOpaque);
  f.draw();
  CHECK(f.at(0, 0) == kSolid && f.at(8, 0) == (12 | kSolid) && f.at(12, 0) == (26 | kSolid));
  // Down an edge too: the bottom is four times as far away. Half way down,
  // 512 * 0x800 * 2 / (0x2000 * 2 + 0x800 * 2) is 102 512ths of 504.
  f.clear();
  f.polygon({Fixture::vertex(0, 0, kBlack, kMiddle, 0x1000), Fixture::vertex(0, 4, kRed, kMiddle, 0x4000),
             Fixture::vertex(16, 4, kRed, kMiddle, 0x4000), Fixture::vertex(16, 0, kBlack, kMiddle, 0x1000)},
            kOpaque);
  f.draw();
  CHECK(f.at(0, 0) == kSolid && f.at(0, 2) == (12 | kSolid) && f.at(9, 2) == (12 | kSolid));
}

TEST(renderer_translucent_polygons_are_blended_over_what_is_there) {
  Fixture f;
  f.control(PolygonRenderer::kAlphaBlending);
  // Sent first, drawn last: alpha 15 of 31 over red.
  f.rectangle(4, 0, 12, 8, kBlue, alpha(15) | number(3), 0x200000);
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x400000);
  f.draw();
  CHECK(f.at(0, 0) == (kRed | kSolid));
  // 63 * 32 / 63 of the red is left, and 63 * 31 / 63 of the blue comes in.
  CHECK(f.at(4, 0) == rgba(32, 0, 31, 63));
  // Over nothing, it is itself, with its alpha.
  CHECK(f.at(8, 0) == rgba(0, 0, 63, 31));
  // Behind the red, it is not drawn.
  f.clear();
  f.rectangle(4, 0, 12, 8, kBlue, alpha(15), 0x600000);
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x400000);
  f.draw();
  CHECK(f.at(4, 0) == (kRed | kSolid) && f.at(8, 0) == rgba(0, 0, 63, 31));
  // With blending off, it replaces what is there.
  f.control(0);
  f.clear();
  f.rectangle(4, 0, 12, 8, kBlue, alpha(15), 0x200000);
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x400000);
  f.draw();
  CHECK(f.at(4, 0) == rgba(0, 0, 63, 31));
}

TEST(renderer_translucent_pixels_of_one_polygon_number_are_drawn_once) {
  Fixture f;
  f.control(PolygonRenderer::kAlphaBlending);
  f.rectangle(0, 0, 16, 8, kRed, kOpaque, 0x400000);
  f.rectangle(0, 0, 12, 8, kBlue, alpha(15) | number(3), 0x200000);
  f.rectangle(4, 0, 8, 8, kGreen, alpha(15) | number(3), 0x100000);  // The same number: left out.
  f.rectangle(8, 0, 16, 8, kGreen, alpha(15) | number(4), 0x100000);  // Another number.
  f.draw();
  CHECK(f.at(0, 0) == rgba(32, 0, 31, 63) && f.at(4, 0) == rgba(32, 0, 31, 63));
  // Green over the blend: 32 * 32 / 63 red, 63 * 31 / 63 green, 31 * 32 / 63 blue.
  CHECK(f.at(8, 0) == rgba(16, 31, 15, 63));
  CHECK(f.at(12, 0) == rgba(32, 31, 0, 63));

  // A translucent polygon leaves the depth as it is, unless it asks to set
  // it: then what is behind it is not drawn afterwards.
  f.clear();
  f.rectangle(0, 0, 8, 8, kBlue, alpha(15) | number(3), 0x200000);
  f.rectangle(8, 0, 16, 8, kBlue, alpha(15) | number(3) | kNewDepth, 0x200000);
  f.rectangle(0, 0, 16, 8, kGreen, alpha(15) | number(4), 0x300000);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 31, 32, 31) && f.at(8, 0) == rgba(0, 0, 63, 31));
}

TEST(renderer_alpha_test) {
  Fixture f;
  f.control(PolygonRenderer::kAlphaBlending | PolygonRenderer::kAlphaTest);
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, 15);
  // Only what has more alpha than the reference is drawn.
  f.rectangle(0, 0, 8, 8, kBlue, alpha(15));
  f.rectangle(8, 0, 16, 8, kBlue, alpha(16));
  f.rectangle(16, 0, 24, 8, kBlue, kOpaque);
  f.draw();
  CHECK(!f.drawn(0, 0) && f.at(8, 0) == rgba(0, 0, 63, 33) && f.at(16, 0) == (kBlue | kSolid));
  f.control(PolygonRenderer::kAlphaBlending);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 0, 63, 31));
}

TEST(renderer_alpha_0_is_an_outline) {
  Fixture f;
  f.rectangle(10, 10, 20, 20, kRed, alpha(0));
  f.draw();
  // The top and bottom lines all across, the others at their two ends; solid.
  CHECK(f.drawn_on_line(10) == 10 && f.drawn_on_line(19) == 10);
  for (int y = 11; y < 19; ++y) CHECK(f.drawn_on_line(y) == 2);
  CHECK(f.at(10, 15) == (kRed | kSolid) && f.at(19, 15) == (kRed | kSolid) && !f.drawn(15, 15));
}

TEST(renderer_textures_with_colours_as_such) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  // Eight by eight texels: red is the column, green the row.
  for (u32 t = 0; t < 8; ++t)
    for (u32 s = 0; s < 8; ++s) f.texture_word((t * 8 + s) * 2, 0x8000 | s | t << 5);
  f.texture_word((2 * 8 + 2) * 2, 2 | 2u << 5);  // One texel without its alpha bit.
  f.hand_over_textures();
  f.textured_square(8, format(7));
  f.draw();
  // A white vertex colour leaves the texel as it is.
  CHECK(f.at(0, 0) == kSolid && f.at(3, 5) == rgba(7, 11, 0, 63) && f.at(7, 7) == rgba(15, 15, 0, 63));
  CHECK(!f.drawn(2, 2) && f.drawn_in_all() == 63);
  // Another vertex colour multiplies it: (7 + 1) * (31 + 1) - 1 over 64.
  f.clear();
  f.textured_square(8, format(7), kOpaque, 0, rgb(31, 63, 0));
  f.draw();
  CHECK(f.at(3, 5) == rgba(3, 11, 0, 63));
  // With textures switched off, the vertex colour is all.
  f.control(0);
  f.draw();
  CHECK(f.at(3, 5) == rgba(31, 63, 0, 63) && f.drawn(2, 2));
  // Where no bank is, nothing is drawn.
  f.control(PolygonRenderer::kTextures);
  f.video.set_control(VideoMemory::kA, 0);
  f.draw();
  CHECK(f.drawn_in_all() == 0);
}

TEST(renderer_textures_outside_their_edges) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  for (u32 t = 0; t < 8; ++t)
    for (u32 s = 0; s < 8; ++s) f.texture_word((t * 8 + s) * 2, 0x8000 | s | t << 5);
  f.hand_over_textures();
  // 16 pixels across the eight texels, and eight more to the left of them.
  const auto wide = [&](u32 texture) {
    f.clear();
    f.polygon({Fixture::vertex(0, 0, kWhite, kMiddle, kOne, -128, 0), Fixture::vertex(0, 4, kWhite, kMiddle, kOne, -128, 64),
               Fixture::vertex(24, 4, kWhite, kMiddle, kOne, 256, 64), Fixture::vertex(24, 0, kWhite, kMiddle, kOne, 256, 0)},
              kOpaque, texture);
    f.draw();
  };
  const auto red = [&](int x) { return f.at(x, 1) & 63; };
  // Held at the edges.
  wide(format(7));
  CHECK(red(0) == 0 && red(7) == 0 && red(9) == 3 && red(15) == 15 && red(16) == 15 && red(23) == 15);
  // Repeated.
  wide(format(7) | kRepeatS);
  CHECK(red(0) == 0 && red(1) == 3 && red(7) == 15 && red(8) == 0 && red(15) == 15 && red(17) == 3);
  // Repeated, every other time mirrored.
  wide(format(7) | kRepeatS | kFlipS);
  CHECK(red(0) == 15 && red(1) == 13 && red(7) == 0 && red(8) == 0 && red(15) == 15 && red(16) == 15 && red(17) == 13);
}

TEST(renderer_textures_with_palettes) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  // 256 colours: a byte a texel. The top line is colours 0, 1, 2, 3...
  for (u32 s = 0; s < 8; ++s) f.texture_byte(s, s);
  // 16 colours, at 0x100: four bits a texel, the low four first.
  f.texture_byte(0x100, 0x10);
  f.texture_byte(0x101, 0x32);
  // Four colours, at 0x200: two bits a texel, the low two first.
  f.texture_byte(0x200, 0xE4);
  // The palette: colour n is red n. From byte 16 on: green n. From 8: blue.
  for (u32 n = 0; n < 4; ++n) {
    f.palette_word(n * 2, n + 1);
    f.palette_word(16 + n * 2, (n + 1) << 5);
  }
  f.palette_word(8, 1u << 10);
  f.palette_word(10, 2u << 10);
  f.hand_over_textures();

  f.textured_square(8, format(4));
  f.draw();
  CHECK(f.at(0, 0) == rgba(3, 0, 0, 63) && f.at(1, 0) == rgba(5, 0, 0, 63) && f.at(3, 0) == rgba(9, 0, 0, 63));
  // Colour 0 can be made transparent.
  f.clear();
  f.textured_square(8, format(4) | kTransparent0);
  f.draw();
  CHECK(!f.drawn(0, 0) && f.at(1, 0) == rgba(5, 0, 0, 63));
  // The palette's place is given in units of 16 bytes,
  f.clear();
  f.textured_square(8, format(4), kOpaque, 1);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 3, 0, 63) && f.at(2, 0) == rgba(0, 7, 0, 63));
  f.clear();
  f.textured_square(8, format(3) | (0x100 >> 3), kOpaque, 1);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 3, 0, 63) && f.at(1, 0) == rgba(0, 5, 0, 63) && f.at(2, 0) == rgba(0, 7, 0, 63) &&
        f.at(3, 0) == rgba(0, 9, 0, 63));
  // and of 8 for four colours.
  f.clear();
  f.textured_square(8, format(2) | (0x200 >> 3), kOpaque, 1);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 0, 3, 63) && f.at(1, 0) == rgba(0, 0, 5, 63));
  f.clear();
  f.textured_square(8, format(2) | (0x200 >> 3), kOpaque, 0);
  f.draw();
  CHECK(f.at(0, 0) == rgba(3, 0, 0, 63) && f.at(1, 0) == rgba(5, 0, 0, 63) && f.at(2, 0) == rgba(7, 0, 0, 63) &&
        f.at(3, 0) == rgba(9, 0, 0, 63));
}

TEST(renderer_textures_with_an_alpha_of_their_own) {
  Fixture f;
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kAlphaBlending);
  f.fill_textures();
  // Five bits of colour number and three of alpha: 7 is solid, 3 is 13 of
  // 31, 0 is nothing.
  f.texture_byte(0, 1 | 7u << 5);
  f.texture_byte(1, 2 | 3u << 5);
  f.texture_byte(2, 2 | 0u << 5);
  // Three bits of colour number and five of alpha, at 0x100.
  f.texture_byte(0x100, 1 | 31u << 3);
  f.texture_byte(0x101, 2 | 13u << 3);
  f.palette_word(2, 0x001F);
  f.palette_word(4, 0x03E0);
  f.hand_over_textures();
  f.textured_square(8, format(1));
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == rgba(0, 63, 0, 27) && !f.drawn(2, 0));
  f.clear();
  f.textured_square(8, format(6) | (0x100 >> 3));
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == rgba(0, 63, 0, 27));
  // Such a polygon counts as translucent: it is blended over what is there,
  // by the texel's alpha. 63 * 36 / 63 blue is left, 63 * 27 / 63 green comes in.
  f.clear();
  f.textured_square(8, format(1));
  f.rectangle(0, 0, 8, 8, kBlue, kOpaque, 0x500000);
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == rgba(0, 27, 36, 63) && f.at(2, 0) == (kBlue | kSolid));
}

TEST(renderer_textures_in_blocks_of_four_by_four) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  // The first block's top row: colours 0, 1, 2, 3.
  f.texture_byte(0, 0xE4);
  // Its four colours start at palette entry 8 (the table counts in fours of
  // bytes): bright red, black, green, blue.
  f.palette_word(16, 0x001F);
  f.palette_word(18, 0x0000);
  f.palette_word(20, 0x03E0);
  f.palette_word(22, 0x7C00);
  const auto with_table_entry = [&](u32 entry) {
    f.video.set_control(VideoMemory::kB, VideoMemory::kEnable);
    f.second_slot_word(0, entry);
    f.hand_over_textures();
    f.clear();
    f.textured_square(8, format(5));
    f.draw();
  };
  // Three colours and a transparent one.
  with_table_entry(4);
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == kSolid && f.at(2, 0) == rgba(0, 63, 0, 63) && !f.drawn(3, 0));
  // Two colours, their mean, and a transparent one.
  with_table_entry(4 | 1u << 14);
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == kSolid && f.at(2, 0) == rgba(31, 0, 0, 63) && !f.drawn(3, 0));
  // Four colours.
  with_table_entry(4 | 2u << 14);
  CHECK(f.at(2, 0) == rgba(0, 63, 0, 63) && f.at(3, 0) == rgba(0, 0, 63, 63));
  // Two colours and two between them, at 3/8 and 5/8.
  with_table_entry(4 | 3u << 14);
  CHECK(f.at(0, 0) == rgba(63, 0, 0, 63) && f.at(1, 0) == kSolid && f.at(2, 0) == rgba(39, 0, 0, 63) &&
        f.at(3, 0) == rgba(23, 0, 0, 63));
  // Without the table's bank, nothing.
  f.video.set_control(VideoMemory::kB, 0);
  f.draw();
  CHECK(f.drawn_in_all() == 0);
}

TEST(renderer_decal_and_toon) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  f.texture_word(0, 0x8000 | 0x03E0);  // Green, solid.
  f.texture_word(2, 0x03E0);           // Green, without its alpha bit.
  f.texture_word(4, 0x8000 | 0x000F);  // Half red.
  f.hand_over_textures();
  // A decal: the texel over the vertex colour by the texel's alpha, with the
  // polygon's alpha. 63 * 63 / 64 either way.
  f.textured_square(8, format(7), kOpaque | mode(1), 0, kRed);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 62, 0, 63) && f.at(1, 0) == rgba(62, 0, 0, 63));

  // The toon table is looked up by half the vertex colour's red: entry 31
  // for full red, 15 for 31.
  f.io.write(PolygonRenderer::kToonTable + 62, 2, 0x7C00);  // Blue.
  f.io.write(PolygonRenderer::kToonTable + 30, 2, 0x001F);  // Red.
  // Without a texture: the table's colour instead of the vertex colour,
  f.clear();
  f.rectangle(0, 0, 8, 8, rgb(63, 63, 0), kOpaque | mode(2));
  f.rectangle(8, 0, 16, 8, rgb(31, 0, 0), kOpaque | mode(2));
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 0, 63, 63) && f.at(8, 0) == rgba(63, 0, 0, 63));
  // or, as a highlight, added to it.
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kHighlight);
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 63, 63, 63) && f.at(8, 0) == rgba(63, 0, 0, 63));
  // With a texture: the texel times the table's colour,
  f.control(PolygonRenderer::kTextures);
  f.clear();
  f.textured_square(8, format(7), kOpaque | mode(2), 0, rgb(31, 63, 63));
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 0, 0, 63) && f.at(2, 0) == rgba(31, 0, 0, 63));
  // or the texel times the vertex colour, plus the table's colour.
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kHighlight);
  f.draw();
  CHECK(f.at(0, 0) == rgba(63, 63, 0, 63) && f.at(2, 0) == rgba(63, 0, 0, 63));
}

TEST(renderer_shadows) {
  Fixture f;
  f.control(PolygonRenderer::kAlphaBlending);
  // A white floor; a mask that is behind the floor from (4, 4) to (12, 12);
  // a black shadow in front of the floor, all over.
  f.rectangle(0, 0, 16, 16, kWhite, kOpaque | number(1), 0x400000);
  f.rectangle(4, 4, 12, 12, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kBlack, alpha(15) | mode(3) | number(2), 0x200000);
  f.draw();
  // The shadow falls where the mask was hidden: 63 * 32 / 63 of the white is left.
  CHECK(f.at(0, 0) == (kWhite | kSolid) && f.at(3, 4) == (kWhite | kSolid) && f.at(12, 12) == (kWhite | kSolid));
  CHECK(f.at(4, 4) == rgba(32, 32, 32, 63) && f.at(11, 11) == rgba(32, 32, 32, 63));
  // A mask that is not hidden casts none.
  f.clear();
  f.rectangle(0, 0, 16, 16, kWhite, kOpaque | number(1), 0x400000);
  f.rectangle(4, 4, 12, 12, kBlack, alpha(15) | mode(3) | number(0), 0x300000);
  f.rectangle(0, 0, 16, 16, kBlack, alpha(15) | mode(3) | number(2), 0x200000);
  f.draw();
  CHECK(f.at(4, 4) == (kWhite | kSolid));
  // A shadow does not fall on a pixel of its own polygon number.
  f.clear();
  f.rectangle(0, 0, 16, 16, kWhite, kOpaque | number(2), 0x400000);
  f.rectangle(4, 4, 12, 12, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kBlack, alpha(15) | mode(3) | number(2), 0x200000);
  f.draw();
  CHECK(f.at(4, 4) == (kWhite | kSolid));
  // Masks that come after a shadow start anew, on the lines they are on:
  // the first mask no longer counts there.
  f.clear();
  f.rectangle(0, 0, 16, 16, kWhite, kOpaque | number(1), 0x400000);
  f.rectangle(0, 0, 4, 8, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kBlack, alpha(15) | mode(3) | number(2), 0x200000);
  f.rectangle(8, 0, 12, 4, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kRed, alpha(15) | mode(3) | number(3), 0x200000);
  f.draw();
  CHECK(f.at(0, 0) == rgba(32, 32, 32, 63) && f.at(5, 0) == (kWhite | kSolid) && f.at(8, 0) == rgba(63, 32, 32, 63));
  // On the lines below the second mask the first still counts, for both
  // shadows: red over the darkened floor, 32 + 31 * 31 / 63 and 32 * 32 / 63.
  CHECK(f.at(0, 4) == rgba(47, 16, 16, 63) && f.at(8, 4) == (kWhite | kSolid));
}

TEST(renderer_fog) {
  Fixture f;
  f.control(PolygonRenderer::kFog);
  f.io.write(PolygonRenderer::kFogColour, 4, 0x7FFF | 31u << 16);
  f.io.write(PolygonRenderer::kFogOffset, 2, 0x1000);
  f.io.write(PolygonRenderer::kFogTable + 3, 1, 32);
  f.io.write(PolygonRenderer::kFogTable + 4, 1, 96);
  // A depth of 0x440000 is 0x2200 in the fog's units, 0x1200 past the
  // offset: four and a half steps of 0x400, so half way between the table's
  // entries 3 and 4, a density of 64 of 128.
  f.rectangle(0, 0, 8, 8, kBlack, kOpaque | kFogged, 0x440000);
  f.rectangle(8, 0, 16, 8, kBlack, kOpaque, 0x440000);  // Takes no fog.
  // Short of the offset: the first entry (0 here).
  f.rectangle(16, 0, 24, 8, kBlack, kOpaque | kFogged, 0x100000);
  f.draw();
  CHECK(f.at(0, 0) == rgba(31, 31, 31, 63) && f.at(8, 0) == kSolid && f.at(16, 0) == kSolid);
  // With smaller steps the same depth is past the last entry.
  f.control(PolygonRenderer::kFog | 4u << PolygonRenderer::kFogShift);
  f.io.write(PolygonRenderer::kFogTable + 31, 1, 127);
  f.draw();
  // A density of 127 counts as all.
  CHECK(f.at(0, 0) == rgba(63, 63, 63, 63));
  // The fog can be told to change only the alpha.
  f.control(PolygonRenderer::kFog | PolygonRenderer::kFogAlphaOnly);
  f.io.write(PolygonRenderer::kFogColour, 4, 0x7FFF);
  f.draw();
  CHECK(f.at(0, 0) == rgba(0, 0, 0, 31) && f.at(8, 0) == kSolid);
  // The clear plane takes fog if its colour says so.
  f.clear();
  f.control(PolygonRenderer::kFog | 4u << PolygonRenderer::kFogShift);
  f.io.write(PolygonRenderer::kFogColour, 4, 0x001F | 31u << 16);
  f.io.write(PolygonRenderer::kClearColour, 4, 0x8000 | 31u << 16);
  f.draw();
  CHECK(f.at(100, 100) == rgba(63, 0, 0, 63));
}

TEST(renderer_edge_marking) {
  Fixture f;
  f.control(PolygonRenderer::kEdgeMarking);
  f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);  // For polygon numbers 8 to 15: green.
  f.rectangle(10, 10, 20, 20, kRed, kOpaque | number(8));
  f.rectangle(0, 30, 10, 40, kRed, kOpaque | number(9));
  f.draw();
  // The pixels around the polygon's rim, next to something of another
  // number that is further away, get the edge colour; the inside does not.
  CHECK(f.at(10, 15) == (kGreen | kSolid) && f.at(19, 15) == (kGreen | kSolid));
  CHECK(f.at(15, 10) == (kGreen | kSolid) && f.at(15, 19) == (kGreen | kSolid));
  CHECK(f.at(11, 11) == (kRed | kSolid) && f.at(18, 18) == (kRed | kSolid));
  // Past the screen's edge is the clear plane.
  CHECK(f.at(0, 35) == (kGreen | kSolid) && f.at(1, 35) == (kRed | kSolid));
  // Next to the same number, or to something nearer, there is no edge.
  f.io.write(PolygonRenderer::kClearColour, 4, 8u << 24);
  f.draw();
  CHECK(f.at(10, 15) == (kRed | kSolid) && f.at(0, 35) == (kGreen | kSolid));
  f.io.write(PolygonRenderer::kClearColour, 4, 0);
  f.io.write(PolygonRenderer::kClearDepth, 2, 0x2001);
  f.clear();
  f.rectangle(10, 10, 20, 20, kRed, kOpaque | number(8), 0x400000);
  f.rectangle(0, 0, 10, 30, kBlue, kOpaque | number(1), 0x300000);
  f.draw();
  // The clear plane is behind it at 0x400200, the blue polygon in front.
  CHECK(f.at(10, 15) == (kRed | kSolid) && f.at(19, 15) == (kGreen | kSolid));
}

TEST(renderer_anti_aliasing) {
  Fixture f;
  f.control(PolygonRenderer::kAntiAliasing);
  // The left edge runs from (10, 0) to (14, 8): half a pixel a line.
  const auto shape = [&] {
    f.polygon({Fixture::vertex(10, 0, kRed, 0x200000), Fixture::vertex(14, 8, kRed, 0x200000),
               Fixture::vertex(20, 8, kRed, 0x200000), Fixture::vertex(20, 0, kRed, 0x200000)},
              kOpaque);
  };
  shape();
  f.draw();
  // On odd lines the edge covers half of its pixel: with nothing behind,
  // that becomes the pixel's alpha.
  CHECK(f.at(10, 0) == (kRed | kSolid) && f.at(10, 1) == rgba(63, 0, 0, 31) && f.at(11, 1) == (kRed | kSolid));
  CHECK(f.at(11, 2) == (kRed | kSolid) && f.at(11, 3) == rgba(63, 0, 0, 31) && f.at(12, 3) == (kRed | kSolid));
  CHECK(f.at(19, 1) == (kRed | kSolid) && !f.drawn(9, 1));
  // With something behind, the two are mixed: 31 of 63 red, 32 of 63 blue.
  f.clear();
  f.rectangle(0, 0, 40, 20, kBlue, kOpaque, 0x400000);
  shape();
  f.draw();
  CHECK(f.at(10, 1) == rgba(31, 0, 32, 63) && f.at(11, 1) == (kRed | kSolid) && f.at(9, 1) == (kBlue | kSolid));
  // Without anti-aliasing the pixel is plain.
  f.control(0);
  f.draw();
  CHECK(f.at(10, 1) == (kRed | kSolid));
}

TEST(renderer_textures_can_be_smoothed_in_3d_scenes) {
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  // As in renderer_textures_with_colours_as_such: red is the column, green
  // the row, and texel (2, 2) is not there.
  for (u32 t = 0; t < 8; ++t)
    for (u32 s = 0; s < 8; ++s) f.texture_word((t * 8 + s) * 2, 0x8000 | s | t << 5);
  f.texture_word((2 * 8 + 2) * 2, 2 | 2u << 5);
  f.hand_over_textures();
  CHECK(f.r.texture_filter() == PolygonRenderer::TextureFilter::Nearest);
  f.textured_square(8, format(7));
  f.draw();
  const PolygonRenderer::Image nearest = f.r.image();

  // Pixel (x, y) is at texture coordinates (16x, 16y), in sixteenths: the
  // corner the four texels around it share, whose middles are half a texel
  // away. Each is a quarter. Red at (3, 5): texels 2 and 3, made six bits,
  // 5 and 7, half each: 6. Green: rows 4 and 5, 9 and 11: 10.
  f.r.set_texture_filter(PolygonRenderer::TextureFilter::Bilinear);
  f.frame.polygons.back().flat = false;
  f.draw();
  CHECK(f.at(3, 5) == rgba(6, 10, 0, 63));
  // At the edge the texels are held: (0, 0) is texel (0, 0) four times.
  CHECK(f.at(0, 0) == kSolid);
  // Texel (2, 2) is not there. Pixel (3, 3) has three quarters of the four
  // there, so it is drawn, from those three: red 64 * (7 + 5 + 7) / 192 and
  // green 64 * (5 + 7 + 7) / 192, rounded, 6 each. Opaque: a texture whose
  // texels are there or not stays so.
  CHECK(f.at(3, 3) == rgba(6, 6, 0, 63));
  CHECK(f.at(2, 2) == rgba(4, 4, 0, 63));
  CHECK(f.drawn_in_all() == 64);

  // A polygon without perspective, as a HUD is drawn, is not smoothed.
  f.frame.polygons.back().flat = true;
  f.draw();
  CHECK(f.r.image() == nearest);
}

TEST(renderer_smoothing_keeps_outlines_and_mixes_alpha) {
  Fixture f;
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kAlphaBlending);
  f.fill_textures();
  // One texel there, in eight by eight: under half of what any pixel is
  // mixed from, so nothing is drawn.
  f.texture_word((2 * 8 + 2) * 2, 0x8000 | 31);
  f.hand_over_textures();
  f.r.set_texture_filter(PolygonRenderer::TextureFilter::Bilinear);
  f.textured_square(8, format(7));
  f.draw();
  CHECK(f.drawn_in_all() == 0);
  // Half there: texels 4 to 7 of each row. Pixel 4 is half texel 3 and half
  // texel 4, so it is drawn; pixel 3 is not: as many as without smoothing.
  f.clear();
  f.fill_textures();
  for (u32 t = 0; t < 8; ++t)
    for (u32 s = 0; s < 8; ++s) f.texture_word((t * 8 + s) * 2, s >= 4 ? 0x8000 | 31 : 0);
  f.hand_over_textures();
  f.textured_square(8, format(7));
  f.draw();
  CHECK(f.drawn_on_line(3) == 4 && f.drawn(4, 3) && !f.drawn(3, 3) && f.at(4, 3) == rgba(63, 0, 0, 63));

  // An alpha of their own: mixed by weight and alpha. Texel 0 is red and
  // solid (63), texel 1 green at 27; pixel (1, 0) is half of each, the row
  // above being held at row 0. Alpha (128 * 63 + 128 * 27) / 256 = 45, red
  // 128 * 63 * 63 / 11520 = 44 and green 128 * 27 * 63 / 11520 = 19,
  // rounded.
  f.clear();
  f.fill_textures();
  for (u32 i = 0; i < 64; ++i) f.texture_byte(i, 0);
  f.texture_byte(0, 1 | 7u << 5);
  f.texture_byte(1, 2 | 3u << 5);
  f.palette_word(2, 0x001F);
  f.palette_word(4, 0x03E0);
  f.hand_over_textures();
  f.textured_square(8, format(1));
  f.draw();
  CHECK(f.at(1, 0) == rgba(44, 19, 0, 45));
}

TEST(renderer_scene_has_shadows_for_the_graphics_hardware) {
  using Scene = bd::nds::HardwareScene;
  using Kind = Scene::Kind;
  Fixture f;
  f.control(PolygonRenderer::kAlphaBlending);
  // A floor; two masks and a shadow; a mask after it, and another shadow.
  f.rectangle(0, 0, 16, 16, kWhite, kOpaque | number(1), 0x400000);
  f.rectangle(4, 4, 12, 12, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(4, 4, 8, 8, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kRed, alpha(15) | mode(3) | number(2), 0x200000);
  f.rectangle(8, 0, 12, 4, kBlack, alpha(15) | mode(3) | number(0), 0x600000);
  f.rectangle(0, 0, 16, 16, kBlack, alpha(15) | mode(3) | number(3), 0x200000);
  const Scene& scene = f.r.build_scene(f.frame, f.video);
  // A run of masks starts with no pixel marked; a shadow first unmarks the
  // pixels of its own number, then is drawn where marked.
  const Kind kinds[] = {Kind::Opaque, Kind::ShadowClear, Kind::ShadowMask, Kind::ShadowGuard, Kind::Shadow,
                        Kind::ShadowClear, Kind::ShadowMask, Kind::ShadowGuard, Kind::Shadow};
  CHECK(scene.runs.size() == std::size(kinds));
  for (std::size_t i = 0; i < std::min(scene.runs.size(), std::size(kinds)); ++i) CHECK(scene.runs[i].kind == kinds[i]);
  if (scene.runs.size() != std::size(kinds)) return;
  CHECK(scene.runs[0].number == 1 && scene.runs[2].count == 12 && scene.runs[3].number == 2 && scene.runs[4].number == 2);
  CHECK(scene.runs[7].number == 3 && scene.runs[8].number == 3);
  // The clear: one triangle over all of clip space.
  const Scene::Vertex* clear = &scene.vertices[scene.runs[1].first];
  CHECK(scene.runs[1].count == 3 && clear[0].position[0] == -1.0f && clear[1].position[0] == 3.0f &&
        clear[2].position[1] == 3.0f && clear[0].position[3] == 1.0f);
  // A mask is never left out for its alpha; a shadow has its own, and is
  // coloured as a modulated polygon.
  CHECK(scene.vertices[scene.runs[2].first].colour[3] == 255);
  const Scene::Vertex& shadow = scene.vertices[scene.runs[4].first];
  CHECK(shadow.colour[3] == (31 << 2 | 31 >> 4) && shadow.colour[0] == 255 && (shadow.info[2] & Scene::kModeMask) == 0);
}

TEST(renderer_scene_draws_the_solid_pixels_of_translucent_polygons_as_opaque_ones) {
  // A pixel of full alpha in a translucent polygon (one with a texture of
  // its own alpha) is drawn as an opaque one, its depth kept, as the
  // processor draws it: each such polygon is first drawn for those pixels,
  // then for the others. Not so where the polygon's own alpha is less.
  using Scene = bd::nds::HardwareScene;
  using Kind = Scene::Kind;
  Fixture f;
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kAlphaBlending);
  f.fill_textures();
  f.hand_over_textures();
  f.textured_square(8, format(1), kOpaque | number(1));
  f.textured_square(8, format(6), kOpaque | number(1) | kNewDepth);
  f.textured_square(8, format(1), alpha(20) | number(1));
  const Scene& scene = f.r.build_scene(f.frame, f.video);
  const Kind kinds[] = {Kind::Solid, Kind::Overlay, Kind::Solid, Kind::Translucent, Kind::Overlay};
  CHECK(scene.runs.size() == std::size(kinds));
  for (std::size_t i = 0; i < std::min(scene.runs.size(), std::size(kinds)); ++i) CHECK(scene.runs[i].kind == kinds[i]);
  if (scene.runs.size() != std::size(kinds)) return;
  CHECK(scene.runs[0].first == 0 && scene.runs[0].count == 6 && scene.runs[1].first == 0 && scene.runs[1].count == 6);
  CHECK(scene.runs[2].first == 6 && scene.runs[3].first == 6 && scene.runs[4].first == 12 && scene.vertices.size() == 18);
}

TEST(renderer_scene_has_fog_for_the_graphics_hardware) {
  using Scene = bd::nds::HardwareScene;
  Fixture f;
  f.control(PolygonRenderer::kFog | PolygonRenderer::kFogAlphaOnly | 3u << PolygonRenderer::kFogShift);
  f.io.write(PolygonRenderer::kFogColour, 4, 0x0010001F);  // Red, alpha 16.
  f.io.write(PolygonRenderer::kFogOffset, 2, 0x1234);
  f.io.write(PolygonRenderer::kClearColour, 4, 0x8000);  // Takes fog.
  for (u32 i = 0; i < 32; ++i) f.io.write(PolygonRenderer::kFogTable + i, 1, i * 4);
  f.rectangle(0, 0, 8, 8, kRed, kOpaque | kFogged);
  f.rectangle(8, 0, 16, 8, kRed, kOpaque);
  const Scene& scene = f.r.build_scene(f.frame, f.video);
  CHECK(scene.fog && scene.fog_alpha_only && scene.clear_fog);
  CHECK(scene.fog_colour[0] == 1.0f && scene.fog_colour[1] == 0.0f && scene.fog_colour[3] == 16.0f / 31.0f);
  CHECK(scene.fog_offset == 0x1234 && scene.fog_step == 0x80 && scene.fog_table[31] == 124);
  // Which polygons take fog.
  CHECK(scene.vertices.size() == 12 && (scene.vertices[0].info[2] & Scene::kFog) != 0 &&
        (scene.vertices[6].info[2] & Scene::kFog) == 0);
  f.control(0);
  CHECK(!f.r.build_scene(f.frame, f.video).fog);
}

TEST(renderer_scene_for_the_graphics_hardware) {
  using Scene = bd::nds::HardwareScene;
  Fixture f;
  f.control(PolygonRenderer::kTextures);
  f.fill_textures();
  for (u32 t = 0; t < 8; ++t)
    for (u32 s = 0; s < 8; ++s) f.texture_word((t * 8 + s) * 2, 0x8000 | s | t << 5);
  f.texture_word((2 * 8 + 2) * 2, 2 | 2u << 5);  // Without its alpha bit.
  f.hand_over_textures();
  f.rectangle(0, 0, 128, 96, kRed, kOpaque);                                   // Opaque: two triangles.
  f.textured_square(8, format(7) | kRepeatS, alpha(15) | number(5));            // Translucent, textured.
  f.rectangle(0, 0, 8, 8, kRed, alpha(0));                                      // An outline: left out.
  const Scene& scene = f.r.build_scene(f.frame, f.video);
  CHECK(scene.serial == 1 && scene.vertices.size() == 12 && scene.runs.size() == 2);
  CHECK(scene.runs[0].kind == Scene::Kind::Opaque && scene.runs[0].first == 0 && scene.runs[0].count == 6);
  CHECK(scene.runs[1].kind == Scene::Kind::Overlay && scene.runs[1].count == 6 && scene.runs[1].number == 5);
  // Places from the top left of the screen to clip space, depth 0 to 1.
  const Scene::Vertex& corner = scene.vertices[0];
  const float w = corner.position[3];
  CHECK(w == 1.0f && corner.position[0] == -1.0f && corner.position[1] == 1.0f && corner.position[2] == 0.25f);
  CHECK(scene.vertices[2].position[0] == 0.0f && scene.vertices[2].position[1] == 0.0f);
  CHECK(corner.colour[0] == 255 && corner.colour[1] == 0 && corner.colour[3] == 255);
  // The textured one: its texture decoded once, to bytes.
  const Scene::Vertex& textured = scene.vertices[6];
  CHECK(textured.colour[3] == (31 << 2 | 31 >> 4) && textured.info[0] == 0 && textured.info[1] == (8u | 8u << 16));
  CHECK(textured.info[2] == (Scene::kTextured | Scene::kRepeatS));
  CHECK(scene.textures.size() == 1 && scene.textures[0].width == 8 && scene.textures[0].height == 8);
  const auto& pixels = *scene.textures[0].pixels;
  CHECK(pixels[5 * 8 + 3] == (28u | 44u << 8 | 255u << 24) && (pixels[2 * 8 + 2] >> 24) == 0);
  // The same texture again is the same decoded picture; changed, another.
  const std::uint64_t key = scene.textures[0].key;
  const auto* decoded = scene.textures[0].pixels.get();
  f.r.build_scene(f.frame, f.video);
  CHECK(scene.serial == 2 && scene.textures[0].key == key && scene.textures[0].pixels.get() == decoded);
  f.fill_textures();
  f.texture_word(0, 0x8000 | 31);
  f.hand_over_textures();
  f.r.build_scene(f.frame, f.video);
  CHECK(scene.textures[0].key != key && ((*scene.textures[0].pixels)[0] & 0xFF) == 255);
  // What is behind: the clear colour and depth, and the alpha under which
  // nothing is drawn (0 always; with the alpha test, up to the reference).
  f.io.write(PolygonRenderer::kClearColour, 4, 31u | 16u << 16);
  f.io.write(PolygonRenderer::kClearDepth, 2, 0x4000);
  f.r.build_scene(f.frame, f.video);
  CHECK(scene.clear_colour[0] == 1.0f && scene.clear_colour[1] == 0.0f && scene.clear_colour[3] == 16.0f / 31.0f);
  CHECK(scene.clear_depth == 0.5f && scene.alpha_threshold == 0.5f / 63.0f);
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kAlphaTest);
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, 10);
  f.r.build_scene(f.frame, f.video);
  CHECK(scene.alpha_threshold == 21.5f / 63.0f);
}

// With w as the depth, the graphics hardware gets w too: the polygon with
// the smaller w is in front, whatever z says, as the processor draws it.
TEST(renderer_scene_depth_is_w_when_the_frame_asks) {
  Fixture f;
  f.frame.depth_is_w = true;
  f.rectangle(0, 0, 8, 8, kRed, kOpaque, 0x100000, 0x100000);
  f.rectangle(0, 0, 8, 8, kBlue, kOpaque, 0x700000, 0x2000);
  f.rectangle(0, 0, 8, 8, kGreen, kOpaque, 0x7FFFFF, 0x7FFFFFFF);  // Past 24 bits: as far as can be.
  const auto& scene = f.r.build_scene(f.frame, f.video);
  REQUIRE(scene.vertices.size() == 18);
  const auto depth = [&](std::size_t i) { return scene.vertices[i].position[2] / scene.vertices[i].position[3]; };
  CHECK(depth(0) == 0x100000 / 16777216.0f && depth(6) == 0x2000 / 16777216.0f && depth(6) < depth(0));
  CHECK(depth(12) == 0xFFFFFF / 16777216.0f);
  // The graphics hardware is told to take w across with perspective.
  CHECK(scene.depth_is_w);
  // With z, z.
  f.frame.depth_is_w = false;
  f.r.build_scene(f.frame, f.video);
  CHECK(!scene.depth_is_w);
  CHECK(depth(0) == 0x100000 / 16777216.0f && depth(6) == 0x700000 / 16777216.0f);
}

TEST(renderer_scene_can_place_vertices_between_pixels) {
  Fixture f;
  Vertex a = Fixture::vertex(10, 20), b = Fixture::vertex(100, 20), c = Fixture::vertex(10, 80);
  a.fine_x = 10 * 16 + 8, a.fine_y = 20 * 16 + 4;  // 10.5 and 20.25.
  b.fine_x = 100 * 16, b.fine_y = 20 * 16;
  c.fine_x = 10 * 16, c.fine_y = 80 * 16;
  f.polygon({a, b, c}, kOpaque);
  // As the DS draws: at whole pixels. This is the default.
  CHECK(!f.r.fine_positions());
  const auto& scene = f.r.build_scene(f.frame, f.video);
  REQUIRE(scene.vertices.size() == 3);
  CHECK(scene.vertices[0].position[0] == 10.0f / 128.0f - 1.0f && scene.vertices[0].position[1] == 1.0f - 20.0f / 96.0f);
  // With the geometry engine's sixteenths, for a larger picture.
  f.r.set_fine_positions(true);
  f.r.build_scene(f.frame, f.video);
  REQUIRE(scene.vertices.size() == 3);
  CHECK(scene.vertices[0].position[0] == 10.5f / 128.0f - 1.0f && scene.vertices[0].position[1] == 1.0f - 20.25f / 96.0f);
  CHECK(scene.vertices[1].position[0] == 100.0f / 128.0f - 1.0f && scene.vertices[2].position[1] == 1.0f - 80.0f / 96.0f);
}

// The edge colour is the table's entry for the polygon number's group of
// eight, as the game writes it: here not black. The graphics hardware gets
// the same table, the clear plane's number, and each corner's number.
TEST(renderer_edge_colour_is_the_table_s_by_polygon_number) {
  Fixture f;
  f.control(PolygonRenderer::kEdgeMarking);
  f.io.write(PolygonRenderer::kEdgeColours + 0, 2, 0x001F);   // 0 to 7: red.
  f.io.write(PolygonRenderer::kEdgeColours + 4, 2, 0x7C00);   // 16 to 23: blue.
  f.io.write(PolygonRenderer::kEdgeColours + 14, 2, 0x03FF);  // 56 to 63: yellow.
  f.rectangle(10, 10, 20, 20, kWhite, kOpaque | number(7));
  f.rectangle(30, 10, 40, 20, kWhite, kOpaque | number(17));
  f.rectangle(50, 10, 60, 20, kWhite, kOpaque | number(63));
  f.io.write(PolygonRenderer::kClearColour, 4, 40u << 24);
  f.draw();
  CHECK(f.at(10, 15) == (kRed | kSolid) && f.at(30, 15) == (kBlue | kSolid) && f.at(50, 15) == (rgb(63, 63, 0) | kSolid));
  CHECK(f.at(15, 15) == (kWhite | kSolid));
  const bd::nds::HardwareScene& scene = f.r.build_scene(f.frame, f.video);
  CHECK(scene.edge_marking && !scene.edge_one_pixel && !scene.anti_aliasing && scene.clear_number == 40);
  CHECK(scene.edge_colours[2][2] == 1.0f && scene.edge_colours[2][0] == 0.0f && scene.edge_colours[7][1] == 1.0f);
  CHECK(scene.vertices[0].info[3] == 7 && scene.vertices[6].info[3] == 17 && scene.vertices[12].info[3] == 63);
}

// Two polygons side by side: only the nearer one's pixels next to the
// other are edges.
TEST(renderer_edge_marking_takes_the_nearer_side) {
  Fixture f;
  f.control(PolygonRenderer::kEdgeMarking);
  f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);  // 8 to 15: green.
  f.io.write(PolygonRenderer::kEdgeColours + 4, 2, 0x7C00);  // 16 to 23: blue.
  f.io.write(PolygonRenderer::kClearColour, 4, 8u << 24);     // The clear plane is as polygon 8.
  f.rectangle(0, 0, 20, 192, kRed, kOpaque | number(8), 0x200000);
  f.rectangle(20, 0, 40, 192, kWhite, kOpaque | number(16), 0x300000);
  f.draw();
  CHECK(f.at(19, 50) == (kGreen | kSolid) && f.at(20, 50) == (kWhite | kSolid));
  // Further away now: the other side.
  f.clear();
  f.rectangle(0, 0, 20, 192, kRed, kOpaque | number(8), 0x300000);
  f.rectangle(20, 0, 40, 192, kWhite, kOpaque | number(16), 0x200000);
  f.draw();
  CHECK(f.at(19, 50) == (kRed | kSolid) && f.at(20, 50) == (kBlue | kSolid));
}

// The width of the outline, by the rule both drawings follow: at 1x one
// pixel either way; at 3x three pixels (one DS pixel) or one.
TEST(renderer_edge_width_at_1x_and_3x) {
  using Marking = bd::nds::EdgeMarking;
  CHECK(PolygonRenderer::edge_width(Marking::OriginalThickness, 1) == 1 && PolygonRenderer::edge_width(Marking::OnePixel, 1) == 1);
  CHECK(PolygonRenderer::edge_width(Marking::OriginalThickness, 3) == 3 && PolygonRenderer::edge_width(Marking::OnePixel, 3) == 1);
  CHECK(PolygonRenderer::edge_width(Marking::Off, 1) == 0 && PolygonRenderer::edge_width(Marking::Off, 3) == 0);
  // A square 10 DS pixels wide in the middle of a picture 30 wide, drawn at
  // `scale`: polygon 1 in front of the clear plane (0, further away).
  const auto marked_across = [](int scale, Marking marking) {
    const int size = 30 * scale;
    const auto at = [&](int x, int y) {
      const bool inside = x >= 10 * scale && x < 20 * scale && y >= 10 * scale && y < 20 * scale;
      return std::pair<std::uint8_t, std::int32_t>{inside ? 1 : 0, inside ? 100 : 1000};
    };
    int count = 0;
    const int y = 15 * scale;
    for (int x = 0; x < size; ++x)
      count += PolygonRenderer::is_edge(at, size, size, x, y, PolygonRenderer::edge_width(marking, scale), 0, 1000) ? 1 : 0;
    return count;
  };
  // Across the middle line: the left and the right rim.
  CHECK(marked_across(1, Marking::OriginalThickness) == 2 && marked_across(1, Marking::OnePixel) == 2);
  CHECK(marked_across(3, Marking::OriginalThickness) == 6 && marked_across(3, Marking::OnePixel) == 2);
  CHECK(marked_across(1, Marking::Off) == 0 && marked_across(3, Marking::Off) == 0);
}

// "Off": no outlines, even where the frame asks for them.
TEST(renderer_edge_marking_can_be_off) {
  Fixture f;
  f.control(PolygonRenderer::kEdgeMarking);
  f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);
  f.rectangle(10, 10, 20, 20, kRed, kOpaque | number(8));
  f.r.set_edge_marking(bd::nds::EdgeMarking::Off);
  f.draw();
  CHECK(f.at(10, 15) == (kRed | kSolid));
  CHECK(!f.r.build_scene(f.frame, f.video).edge_marking);
  f.r.set_edge_marking(bd::nds::EdgeMarking::OnePixel);
  f.draw();
  CHECK(f.at(10, 15) == (kGreen | kSolid) && f.r.build_scene(f.frame, f.video).edge_one_pixel);
}

// Where outlines or anti-aliasing are asked for, the graphics hardware also
// gets the polygons as the DS draws them, in the order drawn (opaque ones
// first), with the registers as the processor reads them: its own
// rasterizer draws those pixels as the processor does.
TEST(renderer_scene_has_the_polygons_as_the_ds_draws_them) {
  using Scene = bd::nds::HardwareScene;
  Fixture f;
  f.io.write(PolygonRenderer::kClearColour, 4, 0x001F | 31u << 16 | 5u << 24 | 0x8000);
  f.io.write(PolygonRenderer::kClearDepth, 2, 0x1000);
  f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);
  f.io.write(PolygonRenderer::kToonTable + 2, 2, 0x7C00);
  f.rectangle(0, 0, 8, 8, kRed, alpha(16) | number(3));
  f.polygon({Fixture::vertex(10, 20, kGreen, 0x1234, 0x2000, 16, 32), Fixture::vertex(30, 40, kBlue),
             Fixture::vertex(40, 15, kWhite)},
            kOpaque | number(9));
  // Made without outlines or anti-aliasing too: at the DS's resolution
  // every pixel is drawn from it.
  CHECK(f.r.build_scene(f.frame, f.video).raster.polygons.size() == 2);

  f.control(PolygonRenderer::kAntiAliasing | PolygonRenderer::kAlphaTest);
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, 12);
  const Scene::Raster& raster = f.r.build_scene(f.frame, f.video).raster;
  REQUIRE(raster.polygons.size() == 2 && raster.corners.size() == 7);
  CHECK(raster.control == (PolygonRenderer::kAntiAliasing | PolygonRenderer::kAlphaTest) && raster.alpha_reference == 12);
  CHECK(raster.clear_colour == (kRed | kSolid) && raster.clear_depth == 0x1000u << 9 && !raster.clear_translucent);
  CHECK(raster.edge_colours[1] == (kGreen | kSolid) && raster.toon[1] == kBlue);
  // The opaque triangle first.
  const Scene::Raster::Polygon& triangle = raster.polygons[0];
  CHECK(triangle.first == 0 && triangle.size == 3 && triangle.attributes == (kOpaque | number(9)));
  CHECK(triangle.lines == (15u * 16 | 40u * 16 << 16) && (triangle.flags & Scene::Raster::kTextured) == 0);
  const Scene::Raster::Corner& corner = raster.corners[0];
  CHECK(corner.x == 160 && corner.y == 320 && corner.z == 0x1234 && corner.w == 0x2000);
  CHECK(corner.st == (16u | 32u << 16) && corner.colour == kGreen);
  CHECK(raster.polygons[1].first == 3 && raster.polygons[1].size == 4 && raster.polygons[1].attributes == (alpha(16) | number(3)));

  // An uncut polygon of a strip is put around in order; fine positions are
  // kept where asked for.
  f.clear();
  f.rectangle(0, 0, 8, 8, kRed, kOpaque);
  f.frame.polygons.back().strip_order = true;
  f.frame.vertices[0].fine_x = 5;
  f.frame.vertices[2].fine_x = 7;
  f.frame.vertices[3].fine_x = 9;
  f.r.set_fine_positions(true);
  const Scene::Raster& strip = f.r.build_scene(f.frame, f.video).raster;
  REQUIRE(strip.corners.size() == 4);
  CHECK(strip.corners[0].x == 5 && strip.corners[2].x == 9 && strip.corners[3].x == 7);
}

// A frame kept as it was drawn, written to a file, read back and put into
// another renderer draws the same picture.
TEST(renderer_frame_dumped_and_read_back_draws_the_same) {
  Fixture f;
  f.r.keep_inputs(true);
  f.control(PolygonRenderer::kTextures | PolygonRenderer::kAntiAliasing | PolygonRenderer::kEdgeMarking |
            PolygonRenderer::kFog | 5u << PolygonRenderer::kFogShift);
  f.io.write(PolygonRenderer::kClearColour, 4, 0x1234 | 20u << 16 | 7u << 24);
  f.io.write(PolygonRenderer::kEdgeColours + 6, 2, 0x4210);
  f.io.write(PolygonRenderer::kToonTable + 8, 2, 0x1F);
  f.io.write(PolygonRenderer::kFogTable + 3, 1, 90);
  f.io.write(PolygonRenderer::kFogColour, 4, 0x7FFF | 9u << 16);
  f.fill_textures();
  for (u32 i = 0; i < 64; ++i) f.texture_word(i * 2, 0x8000 | i * 0x111);
  for (u32 i = 0; i < 32; ++i) f.texture_byte(0x100 + i, i * 7);
  for (u32 i = 0; i < 16; ++i) f.palette_word(i * 2, i * 0x421);
  f.hand_over_textures();
  f.textured_square(8, format(7) | kRepeatS, kOpaque | number(3) | kFogged);
  f.polygon({Fixture::vertex(20, 10, kRed, 0x300000), Fixture::vertex(40, 60, kGreen, 0x300000),
             Fixture::vertex(70, 5, kBlue, 0x300000)},
            alpha(20) | number(9), format(3) | (0x100 >> 3) | 1u << 20);
  f.frame.depth_is_w = true;
  f.draw();
  const std::vector<u32> picture = f.r.image();

  bd::nds::SceneDump dump;
  dump.inputs = f.r.kept_inputs();
  dump.edge_marking = PolygonRenderer::EdgeMarking::OnePixel;
  dump.scale = 3;
  dump.screens[1].assign(256 * 192, 0x80000000u);
  dump.fills[1] = 1;
  const bd_test::TempDir dir;
  std::string error;
  REQUIRE(bd::nds::write_scene_dump(dir.path() / "scene", dump, error));
  bd::nds::SceneDump back;
  REQUIRE(bd::nds::read_scene_dump(dir.path() / "scene", back, error));
  CHECK(back.edge_marking == PolygonRenderer::EdgeMarking::OnePixel && back.scale == 3 && back.fills[1] == 1 &&
        back.screens[1].size() == 256 * 192 && back.inputs.frame.depth_is_w);
  CHECK(back.inputs.frame.vertices == f.frame.vertices && back.inputs.frame.polygons.size() == 2);

  bd::nds::Arm9Io io;
  PolygonRenderer other;
  other.attach(io);
  VideoMemory video;
  bd::nds::restore_scene_inputs(back.inputs, io, video);
  CHECK(other.draw(back.inputs.frame, video) == picture);
  // Not a dump.
  dir.write("other", "not a scene");
  CHECK(!bd::nds::read_scene_dump(dir.path() / "other", back, error));
}
