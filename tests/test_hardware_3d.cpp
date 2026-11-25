// The graphics hardware's 3D drawing (app/hardware_3d.h) against the
// processor's (nds::PolygonRenderer) at the DS's resolution, where the two
// are to agree: the outlines and the anti-aliased edges. On whatever Vulkan
// device SDL finds; a software one, such as Mesa's lavapipe, will do. Without
// one the tests are skipped.
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "app/hardware_3d.h"
#include "nds/arm9_io.h"
#include "nds/display_engine.h"
#include "nds/geometry_engine.h"
#include "nds/polygon_renderer.h"
#include "nds/scene_dump.h"
#include "nds/video_memory.h"
#include "test.h"

namespace {

using bd::app::Hardware3D;
using bd::nds::GeometryEngine;
using bd::nds::PolygonRenderer;
using bd::nds::VideoMemory;
using Vertex = GeometryEngine::Vertex;
using u32 = std::uint32_t;

constexpr int kWidth = PolygonRenderer::kWidth, kHeight = PolygonRenderer::kHeight;
constexpr u32 alpha(u32 a) { return a << 16; }
constexpr u32 number(u32 n) { return n << 24; }
constexpr u32 kOpaque = alpha(31);
constexpr int kOne = 0x1000;  // A w.

// The graphics hardware, opened once for all the tests: an offscreen
// renderer on a device of SDL's choosing.
struct Gpu {
  SDL_Renderer* renderer = nullptr;
  Hardware3D hardware;
  std::string missing;  // Why there is none.

  static Gpu& get() {
    static Gpu gpu;
    return gpu;
  }

 private:
  Gpu() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      missing = std::string("no video: ") + SDL_GetError();
      return;
    }
    renderer = SDL_CreateGPURenderer(nullptr, nullptr);
    if (renderer == nullptr) {
      missing = std::string("no graphics device: ") + SDL_GetError();
      return;
    }
    if (!hardware.open(renderer, missing)) return;
    missing.clear();
  }
};

Hardware3D& hardware() {
  Gpu& gpu = Gpu::get();
  if (!gpu.hardware.is_open()) SKIP(gpu.missing.c_str());
  return gpu.hardware;
}

struct Frame {
  bd::nds::Arm9Io io;
  PolygonRenderer r;
  VideoMemory video;
  GeometryEngine::Frame frame;

  Frame() {
    r.attach(io);
    io.write(PolygonRenderer::kClearDepth, 2, 0x7FFF);
    r.keep_inputs(std::getenv("BD_FAILED_SCENES") != nullptr);
  }
  void control(u32 value) { io.write(PolygonRenderer::kControl, 2, value); }

  static Vertex vertex(int x, int y, u32 colour, int z, int w = kOne) {
    Vertex v;
    v.x = x, v.y = y, v.z = z, v.w = w;
    v.fine_x = x * 16, v.fine_y = y * 16;
    v.colour = colour;
    return v;
  }
  void polygon(std::initializer_list<Vertex> corners, u32 attributes) { polygon(corners.begin(), corners.size(), attributes); }
  void polygon(const Vertex* c, std::size_t size, u32 attributes, u32 texture = 0, u32 palette = 0) {
    GeometryEngine::Polygon p;
    p.first = static_cast<std::uint16_t>(frame.vertices.size());
    p.size = static_cast<std::uint8_t>(size);
    p.attributes = attributes;
    p.texture = texture;
    p.palette = palette;
    p.clockwise = (c[1].x - c[0].x) * (c[2].y - c[0].y) - (c[1].y - c[0].y) * (c[2].x - c[0].x) > 0;
    frame.vertices.insert(frame.vertices.end(), c, c + size);
    frame.polygons.push_back(p);
  }

  // The processor's picture, as it is drawn: six bits a part.
  std::vector<u32> processor() { return r.draw(frame, video); }
  // The graphics hardware's, at the DS's resolution, as the screen shows
  // it: eight bits a part. The screen is all 3D, or `given`.
  std::vector<u32> graphics_hardware(int scale = 1, const std::vector<u32>* given = nullptr) {
    Hardware3D& h = hardware();
    const std::vector<u32> all_3d(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
    const std::vector<u32>& screen = given != nullptr ? *given : all_3d;
    // The graphics hardware draws a scene again only when its serial is
    // new, and each frame here has a renderer of its own.
    static std::uint64_t last_serial = 0;
    const bd::nds::HardwareScene* scene = nullptr;
    do scene = &r.build_scene(frame, video);
    while (scene->serial <= last_serial);
    last_serial = scene->serial;
    bool failed = false;
    REQUIRE(h.draw(0, screen, scene, 0, nullptr, 0, scale, failed) != nullptr || !failed);
    REQUIRE(!failed);
    std::vector<u32> pixels;
    REQUIRE(h.read_screen(0, pixels));
    REQUIRE(pixels.size() == screen.size() * static_cast<std::size_t>(scale * scale));
    return pixels;
  }
};

// A part of six bits as the screen shows it in eight.
int shown(u32 colour, unsigned shift) { return static_cast<int>((((colour >> shift) & 63) * 255 + 31) / 63); }

// The pixels where the outlines are, as the processor draws them (where
// they change its picture), and how many the graphics hardware has the same
// colour on, give or take one of 255.
struct Outlines {
  int pixels = 0, same = 0;
};
Outlines compare_outlines(Frame& f, u32 control) {
  f.control(control);
  f.r.set_edge_marking(PolygonRenderer::EdgeMarking::Off);
  const std::vector<u32> plain = f.processor();
  f.r.set_edge_marking(PolygonRenderer::EdgeMarking::OriginalThickness);
  const std::vector<u32> outlined = f.processor();
  const std::vector<u32> hardware_pixels = f.graphics_hardware();
  Outlines o;
  for (std::size_t i = 0; i < plain.size(); ++i) {
    if (plain[i] == outlined[i]) continue;
    ++o.pixels;
    bool same = true;
    for (unsigned part = 0; part < 3; ++part) {
      const int want = shown(outlined[i], part * 6);
      const int got = static_cast<int>((hardware_pixels[i] >> (part * 8)) & 255);
      same = same && std::abs(want - got) <= 1;
    }
    o.same += same ? 1 : 0;
  }
  return o;
}

// A small cave: a far wall, a nearer rock and a slanted ledge in front, each
// of its own polygon number, with outlines in dark brown.
void cave(Frame& f) {
  f.io.write(PolygonRenderer::kEdgeColours, 2, 0x0846);       // Numbers 0 to 7.
  f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x0423);   // 8 to 15.
  f.io.write(PolygonRenderer::kClearColour, 4, 0x1084 | 31u << 16 | 0u << 24);
  const u32 wall = 20 | 14 << 6 | 8 << 12, rock = 40 | 30 << 6 | 18 << 12, ledge = 56 | 44 << 6 | 30 << 12;
  f.polygon({Frame::vertex(0, 20, wall, 0x700000), Frame::vertex(0, 150, wall, 0x700000),
             Frame::vertex(256, 170, wall, 0x700000), Frame::vertex(256, 10, wall, 0x700000)},
            kOpaque | number(1));
  f.polygon({Frame::vertex(40, 60, rock, 0x500000), Frame::vertex(25, 140, rock, 0x500000),
             Frame::vertex(150, 150, rock, 0x500000), Frame::vertex(120, 50, rock, 0x500000)},
            kOpaque | number(2));
  f.polygon({Frame::vertex(90, 100, ledge, 0x300000), Frame::vertex(60, 180, ledge, 0x300000),
             Frame::vertex(230, 120, ledge, 0x300000)},
            kOpaque | number(9));
}

}  // namespace

// The outline of a polygon, with anti-aliasing, is half its colour and half
// what is behind it: what the polygon was drawn over, at that pixel.
TEST(hardware_outlines_are_the_processor_s_at_1x) {
  for (const u32 control : {PolygonRenderer::kEdgeMarking, PolygonRenderer::kEdgeMarking | PolygonRenderer::kAntiAliasing}) {
    Frame f;
    cave(f);
    const Outlines o = compare_outlines(f, control);
    std::printf("    %s: %d of %d outline pixels the same\n",
                (control & PolygonRenderer::kAntiAliasing) != 0 ? "anti-aliased" : "plain", o.same, o.pixels);
    CHECK(o.pixels > 300);
    CHECK(o.same == o.pixels);
  }
}

namespace {

// Every pixel of the rasterizer's picture against the processor's; the
// first few that differ are printed.
int compare_raster(Frame& f, u32 control, const char* what) {
  f.control(control);
  const std::vector<u32> wanted = f.processor();
  f.graphics_hardware();
  std::vector<u32> drawn;
  REQUIRE(hardware().read_raster(drawn));
  REQUIRE(drawn.size() == wanted.size());
  int differ = 0;
  for (std::size_t i = 0; i < drawn.size(); ++i) {
    if ((drawn[i] & 0xFFFFFF) == wanted[i]) continue;
    if (++differ == 1) {
      // BD_FAILED_SCENES=dir: each scene that differs is written there, for
      // bd_scene_check.
      if (const char* dir = std::getenv("BD_FAILED_SCENES")) {
        static int written = 0;
        bd::nds::SceneDump dump;
        dump.inputs = f.r.kept_inputs();
        dump.edge_marking = f.r.edge_marking();
        dump.screens[0].assign(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
        dump.fills[0] = 1;
        std::string error;
        const std::filesystem::path path = std::filesystem::path(dir) / (std::string(what) + "-" + std::to_string(++written) + ".scene");
        if (bd::nds::write_scene_dump(path, dump, error)) std::printf("    written to %s\n", path.string().c_str());
      }
    }
    if (differ <= 5)
      std::printf("    %s: (%d, %d) is %06x, not %06x\n", what, static_cast<int>(i % kWidth), static_cast<int>(i / kWidth),
                  drawn[i] & 0xFFFFFF, wanted[i]);
  }
  return differ;
}

}  // namespace

TEST(hardware_rasterizer_draws_as_the_processor_at_1x) {
  for (const u32 control : {PolygonRenderer::kEdgeMarking, PolygonRenderer::kEdgeMarking | PolygonRenderer::kAntiAliasing,
                            PolygonRenderer::kAntiAliasing}) {
    Frame f;
    cave(f);
    CHECK(compare_raster(f, control, "cave") == 0);
  }
}

namespace {

// Scenes of random polygons, with all that the DS draws but outlines-only
// polygons: textures (direct colours, 16 colours, with an alpha of their
// own), translucency, shadows, fog, w as the depth, the alpha test, the
// toon table.
void random_scene(Frame& f, std::mt19937& random, u32 control) {
  const auto any = [&](u32 below) { return static_cast<u32>(random() % below); };
  // Textures: 32 by 32 direct colours at 0, 16 colours at 0x1000, five bits
  // of alpha with eight colours (format 6) at 0x2000.
  f.video.set_control(VideoMemory::kA, VideoMemory::kEnable);
  f.video.set_control(VideoMemory::kE, VideoMemory::kEnable);
  for (u32 i = 0; i < 32 * 32; ++i) f.video.write(0x06800000 + i * 2, 2, any(0x10000));
  for (u32 i = 0; i < 32 * 32 / 2; ++i) f.video.write(0x06801000 + i, 1, any(256));
  for (u32 i = 0; i < 32 * 32; ++i) f.video.write(0x06802000 + i, 1, any(256));
  for (u32 i = 0; i < 16; ++i) f.video.write(0x06880000 + i * 2, 2, any(0x8000));
  f.video.set_control(VideoMemory::kA, VideoMemory::kEnable | 3);
  f.video.set_control(VideoMemory::kE, VideoMemory::kEnable | 3);

  f.control(control | PolygonRenderer::kTextures | (any(2) != 0 ? PolygonRenderer::kAlphaBlending : 0u) |
            (any(4) == 0 ? PolygonRenderer::kAlphaTest : 0u) | (any(2) != 0 ? PolygonRenderer::kHighlight : 0u) |
            (any(2) != 0 ? PolygonRenderer::kFog | any(11) << PolygonRenderer::kFogShift : 0u) |
            (any(4) == 0 ? PolygonRenderer::kFogAlphaOnly : 0u));
  f.frame.depth_is_w = any(3) == 0;
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, any(32));
  f.io.write(PolygonRenderer::kClearColour, 4, any(0x8000) | any(32) << 16 | any(16) << 24 | any(2) << 15);
  f.io.write(PolygonRenderer::kClearDepth, 2, any(3) == 0 ? 0x7FFF : any(0x8000));
  for (u32 i = 0; i < 8; ++i) f.io.write(PolygonRenderer::kEdgeColours + 2 * i, 2, any(0x8000));
  for (u32 i = 0; i < 32; ++i) f.io.write(PolygonRenderer::kToonTable + 2 * i, 2, any(0x8000));
  f.io.write(PolygonRenderer::kFogColour, 4, any(0x8000) | any(32) << 16);
  f.io.write(PolygonRenderer::kFogOffset, 2, any(0x8000));
  for (u32 i = 0; i < 32; ++i) f.io.write(PolygonRenderer::kFogTable + i, 1, any(128));

  const int count = 20 + static_cast<int>(any(40));
  for (int n = 0; n < count; ++n) {
    // Convex: corners around a middle.
    const int size = 3 + static_cast<int>(any(3));
    const double cx = any(257), cy = any(193), radius = 8 + any(90);
    std::vector<double> angles;
    for (int i = 0; i < size; ++i) angles.push_back(any(6283) / 1000.0);
    std::sort(angles.begin(), angles.end());
    if (any(2) != 0) std::reverse(angles.begin(), angles.end());
    const bool perspective = any(2) != 0;
    const int z = static_cast<int>(any(0x1000000));
    Vertex corners[5];
    for (int i = 0; i < size; ++i) {
      Vertex& v = corners[i];
      v.x = std::clamp(static_cast<int>(cx + radius * std::cos(angles[static_cast<std::size_t>(i)])), 0, 256);
      v.y = std::clamp(static_cast<int>(cy + radius * std::sin(angles[static_cast<std::size_t>(i)])), 0, 192);
      v.fine_x = v.x * 16, v.fine_y = v.y * 16;
      v.z = any(16) == 0 ? -512 : std::clamp(z + static_cast<int>(any(0x40000)) - 0x20000, 0, 0xFFFFFF);
      v.w = perspective ? 0x200 + static_cast<int>(any(0x20000)) : kOne;
      v.colour = any(0x40000);
      v.s = static_cast<std::int16_t>(static_cast<int>(any(2048)) - 512);
      v.t = static_cast<std::int16_t>(static_cast<int>(any(2048)) - 512);
    }
    const u32 kind = any(10);
    u32 alpha_part = kind < 6 ? 31 : 1 + any(30);
    u32 mode = any(3);
    u32 number_part = any(16);
    if (kind == 9) {  // A shadow mask and its shadow.
      mode = 3;
      alpha_part = 1 + any(30);
      number_part = any(2) != 0 ? 0 : 1 + any(15);
    }
    const u32 attributes = alpha(alpha_part) | number(number_part) | mode << 4 | (any(2) != 0 ? 1u << 15 : 0u) |
                           (any(8) == 0 ? 1u << 14 : 0u) | (any(2) != 0 ? 1u << 11 : 0u);
    u32 texture = 0;
    switch (any(4)) {
      case 1: texture = 7u << 26 | 2u << 20 | 2u << 23; break;
      case 2: texture = 3u << 26 | 2u << 20 | 2u << 23 | (0x1000 >> 3) | (any(2) != 0 ? 1u << 29 : 0u); break;
      case 3: texture = 6u << 26 | 2u << 20 | 2u << 23 | (0x2000 >> 3); break;
      default: break;
    }
    if (texture != 0) texture |= any(16) << 16;  // Repeating and flipping.
    f.polygon(corners, static_cast<std::size_t>(size), attributes, texture, 0);
  }
}

}  // namespace

TEST(hardware_rasterizer_draws_random_scenes_as_the_processor_at_1x) {
  std::mt19937 random(20261122);
  const u32 controls[] = {PolygonRenderer::kEdgeMarking, PolygonRenderer::kAntiAliasing,
                          PolygonRenderer::kEdgeMarking | PolygonRenderer::kAntiAliasing};
  int failed = 0;
  constexpr int kScenes = 60;
  for (int round = 0; round < kScenes; ++round) {
    Frame f;
    random_scene(f, random, controls[round % 3]);
    if (compare_raster(f, f.r.control(), "random") != 0) ++failed;
  }
  std::printf("    %d of %d scenes drawn the same\n", kScenes - failed, kScenes);
  CHECK(failed == 0);
}

// Fog of the alpha only leaves the colour as it is.
TEST(hardware_rasterizer_fogs_as_the_processor) {
  for (const u32 alpha_only : {0u, PolygonRenderer::kFogAlphaOnly}) {
    Frame f;
    f.io.write(PolygonRenderer::kClearColour, 4, 0x1234 | 31u << 16 | 0x8000);
    f.io.write(PolygonRenderer::kClearDepth, 2, 0x4000);
    f.io.write(PolygonRenderer::kFogColour, 4, 0x7C00 | 10u << 16);
    for (u32 i = 0; i < 32; ++i) f.io.write(PolygonRenderer::kFogTable + i, 1, 64);
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(10, 20, 63, 0x100), Frame::vertex(20, 20, 63, 0x100)},
              kOpaque | 1u << 15);
    CHECK(compare_raster(f, PolygonRenderer::kAntiAliasing | PolygonRenderer::kFog | alpha_only | 3u << 8, "fog") == 0);
  }
}

// Larger than the DS's, an outline is drawn from the triangles' own pixels,
// solid in its edge colour, as many pixels wide as a DS pixel is, inside the
// polygon's edges: or one, if asked.
namespace {

// The screen with `scene` drawn by the graphics hardware at `scale`.
std::vector<u32> drawn(const bd::nds::HardwareScene& scene, int scale) {
  Hardware3D& h = hardware();
  const std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
  bool failed = false;
  REQUIRE(h.draw(0, screen, &scene, 0, nullptr, 0, scale, failed) != nullptr || !failed);
  REQUIRE(!failed);
  std::vector<u32> pixels;
  REQUIRE(h.read_screen(0, pixels));
  return pixels;
}

}  // namespace

// Translucent polygons whose textures have pixels of full alpha, drawn
// together where they share no pixel (nds/polygon_outline.h), come out as
// when each is drawn by itself, pixel for pixel: a mesh whose corners and
// edges they share, and spikes over it and over one another.
TEST(hardware_draws_polygons_that_share_no_pixel_together_as_one_by_one) {
  std::mt19937 random(20261124);
  const auto any = [&](u32 below) { return static_cast<u32>(random() % below); };
  for (int round = 0; round < 4; ++round) {
    Frame f;
    for (const auto bank : {VideoMemory::kA, VideoMemory::kE}) f.video.set_control(bank, VideoMemory::kEnable);
    for (u32 i = 0; i < 0x20000; i += 2) f.video.write(0x06800000 + i, 2, any(0x10000));
    for (u32 i = 0; i < 0x10000; i += 2) f.video.write(0x06880000 + i, 2, any(0x10000));
    f.video.set_control(VideoMemory::kA, VideoMemory::kEnable | 3);
    f.video.set_control(VideoMemory::kE, VideoMemory::kEnable | 3);
    f.control(PolygonRenderer::kTextures | PolygonRenderer::kAlphaBlending);
    // Textures with an alpha of their own: three bits (format 1) or five (6).
    const auto texture = [&] { return (any(2) != 0 ? 1u : 6u) << 26 | 2u << 20 | 2u << 23 | any(0x1000) | any(16) << 16; };
    const auto corner = [&](int x, int y) {
      Vertex v = Frame::vertex(std::clamp(x, 0, 256), std::clamp(y, 0, 192), any(0x40000), 0x400000 + static_cast<int>(any(0x100000)),
                               0x1000 + static_cast<int>(any(0x4000)));
      v.s = static_cast<std::int16_t>(any(1024));
      v.t = static_cast<std::int16_t>(any(1024));
      return v;
    };
    // One number and one way with the depth for most, as a model has.
    const u32 number_part = any(64), new_depth = round % 2 != 0 ? 1u << 11 : 0u;
    const auto attributes = [&] { return kOpaque | number(any(16) == 0 ? any(64) : number_part) | new_depth | 3u << 6; };
    const int step = 8 + static_cast<int>(any(12)), columns = 256 / step, rows = 192 / step;
    std::vector<Vertex> grid;
    for (int r = 0; r <= rows; ++r)
      for (int c = 0; c <= columns; ++c) grid.push_back(corner(c * step + static_cast<int>(any(3)) - 1, r * step + static_cast<int>(any(3)) - 1));
    const auto at = [&](int r, int c) { return grid[static_cast<std::size_t>(r * (columns + 1) + c)]; };
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < columns; ++c) {
        const Vertex quad[4] = {at(r, c), at(r + 1, c), at(r + 1, c + 1), at(r, c + 1)};
        f.polygon(quad, 4, attributes(), texture());
      }
    for (int n = 0; n < 60; ++n) {
      const int x = static_cast<int>(any(256)), y = static_cast<int>(any(192)), width = 1 + static_cast<int>(any(20));
      const Vertex spike[3] = {corner(x, y), corner(x + width, y + 30), corner(x - width, y + 30)};
      f.polygon(spike, 3, attributes(), texture());
    }
    const bd::nds::HardwareScene& merged = f.r.build_scene(f.frame, f.video);
    // The same, each triangle in a pair of runs of its own: the triangles of
    // a convex polygon share no pixel, so that is each polygon by itself.
    bd::nds::HardwareScene alone = merged;
    alone.runs.clear();
    std::size_t triangles = 0;
    for (std::size_t i = 0; i < merged.runs.size(); ++i) {
      const auto& run = merged.runs[i];
      const bool pair = run.kind == bd::nds::HardwareScene::Kind::Solid && i + 1 < merged.runs.size();
      if (!pair) {
        alone.runs.push_back(run);
        continue;
      }
      const auto& second = merged.runs[++i];
      for (std::uint32_t v = 0; v < run.count; v += 3) {
        alone.runs.push_back({run.first + v, 3, run.kind, run.depth_equal, run.number});
        alone.runs.push_back({second.first + v, 3, second.kind, second.depth_equal, second.number});
        ++triangles;
      }
    }
    CHECK(merged.runs.size() * 4 < alone.runs.size());
    static std::uint64_t serial = std::uint64_t{1} << 40;
    bd::nds::HardwareScene first = merged;
    first.serial = ++serial;
    alone.serial = ++serial;
    for (const int scale : {2, 3}) {
      const std::vector<u32> together = drawn(first, scale), one_by_one = drawn(alone, scale);
      std::size_t differ = 0;
      for (std::size_t i = 0; i < together.size(); ++i) differ += together[i] != one_by_one[i] ? 1 : 0;
      if (differ != 0)
        std::printf("    round %d at %dx: %zu runs for %zu triangles, %zu pixels differ\n", round, scale, merged.runs.size(),
                    triangles, differ);
      CHECK(differ == 0);
      first.serial = ++serial;
      alone.serial = ++serial;
    }
  }
}

TEST(hardware_outlines_keep_their_thickness_at_3x) {
  for (const auto marking : {PolygonRenderer::EdgeMarking::OriginalThickness, PolygonRenderer::EdgeMarking::OnePixel}) {
    Frame f;
    f.r.set_edge_marking(marking);
    f.control(PolygonRenderer::kEdgeMarking | PolygonRenderer::kAntiAliasing);
    f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);  // Green, for numbers 8 to 15.
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(10, 20, 63, 0x100), Frame::vertex(20, 20, 63, 0x100),
               Frame::vertex(20, 10, 63, 0x100)},
              kOpaque | number(8));
    const std::vector<u32> shown_pixels = f.graphics_hardware(3);
    const auto at = [&](int x, int y) { return shown_pixels[static_cast<std::size_t>(y * kWidth * 3 + x)] & 0xFFFFFF; };
    constexpr u32 kGreen = 0x00FF00, kRed = 0x0000FF;
    const int wide = marking == PolygonRenderer::EdgeMarking::OnePixel ? 1 : 3;
    // Across the middle line, and down the middle column.
    for (int i = 29; i < 62; ++i) {
      const bool inside = i >= 30 && i < 60;
      const bool edge = inside && (i < 30 + wide || i >= 60 - wide);
      CHECK(at(i, 45) == (edge ? kGreen : inside ? kRed : 0u));
      CHECK(at(45, i) == (edge ? kGreen : inside ? kRed : 0u));
    }
  }
}

// Above 1x, outlines are as thick on every side: one DS pixel (4 pixels at
// 4x) across a straight edge and across a slanting one, measured square to
// the edge, soft on the inner side; none along an edge two polygons of one
// number share; and one line, on the nearer group, where two groups touch.
TEST(hardware_outlines_are_even_soft_and_on_the_nearer_group_at_4x) {
  constexpr int kScale = 4, kRow = kWidth * kScale;
  const auto green = [](u32 p) { return static_cast<double>((p >> 8) & 255) / 255.0; };
  const auto blue = [](u32 p) { return static_cast<double>((p >> 16) & 255) / 255.0; };
  {
    // A square, from (10, 10) to (40, 40), and a diamond about (100, 60).
    Frame f;
    f.control(PolygonRenderer::kEdgeMarking);
    f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);  // Green, for numbers 8 to 15.
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(10, 40, 63, 0x100), Frame::vertex(40, 40, 63, 0x100),
               Frame::vertex(40, 10, 63, 0x100)},
              kOpaque | number(8));
    f.polygon({Frame::vertex(100, 30, 63, 0x100), Frame::vertex(70, 60, 63, 0x100), Frame::vertex(100, 90, 63, 0x100),
               Frame::vertex(130, 60, 63, 0x100)},
              kOpaque | number(9));
    const std::vector<u32> pixels = f.graphics_hardware(kScale);
    const auto at = [&](int x, int y) { return pixels[static_cast<std::size_t>(y * kRow + x)]; };
    // Across the square's left edge, on a row through its middle: 4.
    double straight = 0;
    for (int x = 30; x < 120; ++x) straight += green(at(x, 25 * kScale));
    // Across the diamond's left edge, on a row half way down its top half:
    // the edge slants at 45 degrees, so 4 square to it is 4 times the root
    // of 2 along the row.
    double slanting = 0;
    for (int x = 70 * kScale; x < 100 * kScale; ++x) slanting += green(at(x, 45 * kScale));
    std::printf("    across a straight edge %.2f pixels, a slanting one %.2f (%.2f square to it)\n", straight, slanting,
                slanting / std::sqrt(2.0));
    CHECK(std::fabs(straight - kScale) < 0.01);
    CHECK(std::fabs(slanting / std::sqrt(2.0) - kScale) < 0.75);
    // Soft: some pixels along the slanting edge are part outline.
    int part = 0;
    for (int x = 70 * kScale; x < 100 * kScale; ++x) part += green(at(x, 45 * kScale)) > 0.05 && green(at(x, 45 * kScale)) < 0.95;
    CHECK(part >= 1);
  }
  {
    // One number, two triangles: no line along the edge they share.
    Frame f;
    f.control(PolygonRenderer::kEdgeMarking);
    f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(10, 60, 63, 0x100), Frame::vertex(60, 60, 63, 0x100)},
              kOpaque | number(8));
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(60, 60, 63, 0x100), Frame::vertex(60, 10, 63, 0x100)},
              kOpaque | number(8));
    const std::vector<u32> pixels = f.graphics_hardware(kScale);
    int inside = 0;
    for (int i = 20 * kScale; i < 50 * kScale; ++i) inside += green(pixels[static_cast<std::size_t>(i * kRow + i)]) > 0.0;
    CHECK(inside == 0);
  }
  {
    // Two groups side by side, the left one nearer: one line where they
    // meet, the left one's, as wide as any other.
    Frame f;
    f.control(PolygonRenderer::kEdgeMarking);
    f.io.write(PolygonRenderer::kEdgeColours + 2, 2, 0x03E0);  // Green, 8 to 15.
    f.io.write(PolygonRenderer::kEdgeColours + 4, 2, 0x7C00);  // Blue, 16 to 23.
    f.polygon({Frame::vertex(10, 10, 63, 0x100), Frame::vertex(10, 40, 63, 0x100), Frame::vertex(30, 40, 63, 0x100),
               Frame::vertex(30, 10, 63, 0x100)},
              kOpaque | number(8));
    f.polygon({Frame::vertex(30, 10, 63, 0x200), Frame::vertex(30, 40, 63, 0x200), Frame::vertex(50, 40, 63, 0x200),
               Frame::vertex(50, 10, 63, 0x200)},
              kOpaque | number(16));
    const std::vector<u32> pixels = f.graphics_hardware(kScale);
    double near_line = 0, far_line = 0;
    for (int x = 20 * kScale; x < 40 * kScale; ++x) {
      const u32 p = pixels[static_cast<std::size_t>(25 * kScale * kRow + x)];
      near_line += green(p);
      far_line += blue(p);
    }
    std::printf("    where two groups meet: the nearer's line %.2f pixels, the further's %.2f\n", near_line, far_line);
    CHECK(std::fabs(near_line - kScale) < 0.01 && far_line == 0.0);
  }
}

TEST(outlines_are_looked_for_only_near_the_opaque_polygons_shown) {
  // An opaque square from (10, 10) to (40, 40) and a translucent one from
  // (100, 100) to (200, 180), in the DS's pixels; w is 2.
  bd::nds::HardwareScene scene;
  const auto square = [&](float left, float top, float right, float bottom, bd::nds::HardwareScene::Kind kind) {
    bd::nds::HardwareScene::Run run;
    run.first = static_cast<std::uint32_t>(scene.vertices.size());
    run.count = 6;
    run.kind = kind;
    const float corners[6][2] = {{left, top}, {left, bottom}, {right, bottom}, {left, top}, {right, bottom}, {right, top}};
    for (const auto& c : corners) {
      bd::nds::HardwareScene::Vertex v;
      v.position[0] = (c[0] / 128.0f - 1.0f) * 2.0f;
      v.position[1] = (1.0f - c[1] / 96.0f) * 2.0f;
      v.position[2] = 1.0f;
      v.position[3] = 2.0f;
      scene.vertices.push_back(v);
    }
    scene.runs.push_back(run);
  };
  square(10, 10, 40, 40, bd::nds::HardwareScene::Kind::Opaque);
  square(100, 100, 200, 180, bd::nds::HardwareScene::Kind::Translucent);
  using Area = std::array<std::uint32_t, 4>;
  // At 4x: the opaque square only, a pixel more on each side.
  CHECK((Hardware3D::opaque_bounds(scene, 1024, 768) == Area{39, 39, 161, 161}));
  // Where the screen shows the 3D: within a DS pixel of that.
  CHECK((Hardware3D::outline_area(scene, {0, 0, 256, 192}, 4) == Area{39, 39, 161, 161}));
  CHECK((Hardware3D::outline_area(scene, {0, 0, 20, 192}, 4) == Area{39, 39, 84, 161}));
  CHECK((Hardware3D::outline_area(scene, {0, 30, 256, 192}, 4) == Area{39, 116, 161, 161}));
  // Shown away from it, or not at all: nowhere.
  CHECK((Hardware3D::outline_area(scene, {100, 0, 256, 192}, 4) == Area{0, 0, 0, 0}));
  CHECK((Hardware3D::outline_area(scene, {0, 0, 0, 0}, 4) == Area{0, 0, 0, 0}));
  // A corner behind the eye: anywhere.
  scene.vertices[0].position[3] = -1.0f;
  CHECK((Hardware3D::opaque_bounds(scene, 1024, 768) == Area{0, 0, 1024, 768}));
  // No opaque polygon: nowhere.
  scene.runs.erase(scene.runs.begin());
  CHECK((Hardware3D::opaque_bounds(scene, 1024, 768) == Area{0, 0, 0, 0}));
}

namespace {

// What games draw that random polygons seldom are: meshes whose polygons
// share their edges, thin polygons, polygons of up to ten corners, every
// texture format, and many polygons.
void mesh_scene(Frame& f, std::mt19937& random, u32 control) {
  const auto any = [&](u32 below) { return static_cast<u32>(random() % below); };
  for (const auto bank : {VideoMemory::kA, VideoMemory::kB, VideoMemory::kE})
    f.video.set_control(bank, VideoMemory::kEnable);
  for (u32 i = 0; i < 0x20000; i += 2) f.video.write(0x06800000 + i, 2, any(0x10000));  // Bank A, slot 0.
  for (u32 i = 0; i < 0x20000; i += 2) f.video.write(0x06820000 + i, 2, any(0x10000));  // Bank B, slot 1.
  for (u32 i = 0; i < 0x10000; i += 2) f.video.write(0x06880000 + i, 2, any(0x10000));  // Bank E, palettes.
  f.video.set_control(VideoMemory::kA, VideoMemory::kEnable | 3);
  f.video.set_control(VideoMemory::kB, VideoMemory::kEnable | 3 | 8);
  f.video.set_control(VideoMemory::kE, VideoMemory::kEnable | 3);
  f.control(control | PolygonRenderer::kTextures | PolygonRenderer::kAlphaBlending |
            (any(2) != 0 ? PolygonRenderer::kAlphaTest : 0u) | (any(2) != 0 ? PolygonRenderer::kFog | 6u << 8 : 0u));
  f.frame.depth_is_w = any(2) != 0;
  f.io.write(PolygonRenderer::kAlphaTestReference, 2, any(16));
  f.io.write(PolygonRenderer::kClearColour, 4, any(0x8000) | any(32) << 16 | any(64) << 24 | any(2) << 15);
  f.io.write(PolygonRenderer::kFogOffset, 2, any(0x8000));
  for (u32 i = 0; i < 32; ++i) f.io.write(PolygonRenderer::kFogTable + i, 1, i * 4);
  // Forty textures (with the palette, as the polygon gives it), as a scene
  // has: thousands would not fit in the graphics hardware's atlas.
  std::vector<u32> textures;
  for (int i = 0; i < 40; ++i) {
    const u32 format = any(8);
    textures.push_back(format == 0 ? 0u
                                   : format << 26 | any(4) << 20 | any(4) << 23 | any(0x2000) | any(16) << 16 |
                                         (any(2) != 0 ? 1u << 29 : 0u));
  }
  const auto texture = [&] { return textures[any(40)]; };
  const auto corner = [&](int x, int y, int z, int w) {
    Vertex v;
    v.x = std::clamp(x, 0, 256), v.y = std::clamp(y, 0, 192);
    v.fine_x = v.x * 16, v.fine_y = v.y * 16;
    // Corners on the near plane come out of the geometry engine at -512.
    v.z = any(16) == 0 ? -512 : std::clamp(z, 0, 0xFFFFFF), v.w = w;
    v.colour = any(0x40000);
    v.s = static_cast<std::int16_t>(static_cast<int>(any(4096)) - 1024);
    v.t = static_cast<std::int16_t>(static_cast<int>(any(4096)) - 1024);
    return v;
  };
  const bool perspective = any(2) != 0;
  const bool far_away = any(4) == 0;  // w of up to 30 bits.
  const auto w_of = [&] {
    return !perspective ? kOne : far_away ? 0x10000 + static_cast<int>(any(0x3FFF0000)) : 0x1000 + static_cast<int>(any(0x400000));
  };

  // A mesh: a grid of quads, each split into two triangles or kept whole,
  // over most of the screen, its corners shared.
  const int step = 6 + static_cast<int>(any(14));
  const int columns = 256 / step + 1, rows = 192 / step + 1;
  std::vector<Vertex> grid;
  const int base_z = static_cast<int>(any(0x800000));
  for (int r = 0; r <= rows; ++r)
    for (int c = 0; c <= columns; ++c)
      grid.push_back(corner(c * step + static_cast<int>(any(3)) - 1, r * step + static_cast<int>(any(3)) - 1,
                            base_z + static_cast<int>(any(0x10000)), w_of()));
  const u32 mesh_number = any(64), mesh_alpha = any(4) == 0 ? 1 + any(30) : 31;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < columns; ++c) {
      const Vertex a = grid[static_cast<std::size_t>(r * (columns + 1) + c)], b = grid[static_cast<std::size_t>(r * (columns + 1) + c + 1)];
      const Vertex d = grid[static_cast<std::size_t>((r + 1) * (columns + 1) + c)], e = grid[static_cast<std::size_t>((r + 1) * (columns + 1) + c + 1)];
      const u32 attributes = alpha(mesh_alpha) | number(any(3) == 0 ? any(64) : mesh_number) | any(3) << 4 | 1u << 15 | 1u << 11;
      const u32 t = texture();
      if (any(2) != 0) {
        const Vertex quad[4] = {a, d, e, b};
        f.polygon(quad, 4, attributes, t, t & 63);
      } else {
        const Vertex one[3] = {a, d, e}, two[3] = {a, e, b};
        f.polygon(one, 3, attributes, t, t & 63);
        f.polygon(two, 3, attributes, t, t & 63);
      }
    }
  // Thin spikes in front, as stalagmites: a pixel to a few wide.
  for (int n = 0; n < 300; ++n) {
    const int x = static_cast<int>(any(256)), y = static_cast<int>(any(192)), width = 1 + static_cast<int>(any(4));
    const int height = 5 + static_cast<int>(any(60)), z = static_cast<int>(any(0x800000));
    const Vertex spike[3] = {corner(x, y, z, w_of()), corner(x + width, y + height, z, w_of()), corner(x - width, y + height, z, w_of())};
    const u32 t = texture();
    f.polygon(spike, 3, alpha(any(3) == 0 ? 1 + any(30) : 31) | number(any(64)) | any(3) << 4, t, t & 63);
  }
  // Polygons of ten corners.
  for (int n = 0; n < 20; ++n) {
    const double cx = any(256), cy = any(192), radius = 4 + any(60);
    Vertex ring[10];
    const int z = static_cast<int>(any(0x800000));
    for (int i = 0; i < 10; ++i)
      ring[i] = corner(static_cast<int>(cx + radius * std::cos(i * 0.6283)), static_cast<int>(cy + radius * std::sin(i * 0.6283)),
                       z, w_of());
    const u32 t = texture();
    f.polygon(ring, 10, alpha(31) | number(any(64)), t, t & 63);
  }
}

}  // namespace

TEST(hardware_rasterizer_draws_meshes_as_the_processor_at_1x) {
  std::mt19937 random(7);
  const u32 controls[] = {PolygonRenderer::kAntiAliasing, PolygonRenderer::kEdgeMarking | PolygonRenderer::kAntiAliasing,
                          PolygonRenderer::kEdgeMarking};
  int failed = 0;
  const int scenes = std::getenv("BD_SCENES") != nullptr ? std::atoi(std::getenv("BD_SCENES")) : 12;
  for (int round = 0; round < scenes; ++round) {
    Frame f;
    mesh_scene(f, random, controls[round % 3]);
    const int differ = compare_raster(f, f.r.control(), "mesh");
    if (differ != 0) {
      ++failed;
      std::printf("    scene %d: %zu polygons, %d pixels differ\n", round, f.frame.polygons.size(), differ);
    }
  }
  CHECK(failed == 0);
}

namespace {

// The screen as shown at 1x, all 3D, against the processor's picture with
// six bits as eight, as the DS's screens show them: the number of pixels not
// the same. Where the processor's picture has no alpha, the screen's own
// pixel (black) shows.
int compare_shown(Frame& f) {
  const std::vector<u32> wanted = f.processor();
  const std::vector<u32> shown_pixels = f.graphics_hardware();
  int differ = 0;
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    const bool seen = ((wanted[i] >> 18) & 63) != 0;
    for (unsigned part = 0; part < 3; ++part) {
      const u32 six = (wanted[i] >> (part * 6)) & 63;
      const u32 want = seen ? (six << 2 | six >> 4) : 0;
      if (((shown_pixels[i] >> (part * 8)) & 255) != want) {
        ++differ;
        break;
      }
    }
  }
  return differ;
}

}  // namespace

// At the DS's resolution the graphics hardware shows what the processor
// draws, pixel for pixel, with or without outlines and smoothed edges.
TEST(hardware_shows_the_processor_s_scene_at_1x) {
  std::mt19937 random(2611);
  const u32 controls[] = {0, PolygonRenderer::kAntiAliasing, PolygonRenderer::kEdgeMarking};
  int failed = 0, scenes = 0;
  for (int round = 0; round < 12; ++round) {
    Frame f;
    if (round % 4 == 3) mesh_scene(f, random, controls[round % 3]);
    else random_scene(f, random, controls[round % 3]);
    const int differ = compare_shown(f);
    ++scenes;
    if (differ != 0) {
      ++failed;
      std::printf("    scene %d (control %04x): %d pixels differ\n", round, f.r.control(), differ);
    }
  }
  std::printf("    %d of %d scenes shown the same\n", scenes - failed, scenes);
  CHECK(failed == 0);
}

// A display capture of the scene alone blended with a bank of video memory,
// then shown: five bits a part, blended in sixteenths, as the processor's
// capture keeps it in video memory (nds::Display).
TEST(hardware_captures_as_the_processor_at_1x) {
  std::mt19937 random(907);
  Frame f;
  random_scene(f, random, PolygonRenderer::kAntiAliasing);
  const std::vector<u32> wanted = f.processor();
  f.graphics_hardware();
  bd::nds::CaptureRecipe recipe;
  recipe.source = 2;
  recipe.only_3d = true;
  recipe.weight_first = 7;
  recipe.weight_second = 10;
  // Video memory holds five bits a part; the recipe has them as six.
  recipe.second_pixels.resize(0x10000);
  for (u32& p : recipe.second_pixels) {
    const u32 five = random() & 0x7FFF;
    p = (five & 0x1F) << 1 | ((five >> 5) & 0x1F) << 7 | ((five >> 10) & 0x1F) << 13;
  }
  const std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
  Hardware3D& h = hardware();
  REQUIRE(h.capture(f.r.scene(), recipe, screen, 77, 1));
  // Shown as its bank is (display mode 2): each line from its row.
  bd::nds::CaptureLines lines;
  for (int y = 0; y < kHeight; ++y) {
    lines.lines[static_cast<std::size_t>(y)].on = true;
    lines.lines[static_cast<std::size_t>(y)].y0 = y * 256;
  }
  const std::vector<u32> shown(screen.size(), bd::nds::DisplayEngine::kCapturedBelow);
  bool failed = false;
  REQUIRE(h.draw(0, shown, nullptr, 77, &lines, 0, 1, failed) != nullptr);
  std::vector<u32> shown_pixels;
  REQUIRE(h.read_screen(0, shown_pixels));
  int differ = 0;
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    for (unsigned part = 0; part < 3; ++part) {
      const u32 first = (wanted[i] >> (part * 6) & 63) >> 1;
      const u32 second = (recipe.second_pixels[i] >> (part * 6) & 63) >> 1;
      const u32 six = std::min<u32>((first * 7 + second * 10) / 16, 31) << 1;
      if (((shown_pixels[i] >> (part * 8)) & 255) != (six << 2 | six >> 4)) {
        ++differ;
        break;
      }
    }
  }
  std::printf("    %d of %zu pixels differ\n", differ, wanted.size());
  CHECK(differ == 0);
}

// A 2D layer on top blended with the 3D just below it (nds::DisplayEngine::
// kMark3DBelow): by the two weights, as the DS sums them; where the scene
// has no pixel, the 2D layer as it is.
TEST(hardware_blends_a_layer_with_the_3d_below_it_as_the_processor) {
  using bd::nds::DisplayEngine;
  std::mt19937 random(311);
  Frame f;
  random_scene(f, random, 0);
  const std::vector<u32> scene_pixels = f.processor();
  std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight);
  for (u32& p : screen) {
    const u32 weight_top = random() % 17, weight_3d = random() % 17;
    p = (random() & 0x3FFFF) | DisplayEngine::kMark3DBelow | weight_top << DisplayEngine::kMarkFactorShift |
        weight_3d << DisplayEngine::kMarkBelowWeightShift;
  }
  const std::vector<u32> pixels = f.graphics_hardware(1, &screen);
  int differ = 0, blended = 0;
  for (std::size_t i = 0; i < screen.size(); ++i) {
    const u32 p = screen[i], three = scene_pixels[i];
    const u32 weight_top = (p >> DisplayEngine::kMarkFactorShift) & 31, weight_3d = (p >> DisplayEngine::kMarkBelowWeightShift) & 31;
    u32 want = p & 0x3FFFF;
    if (((three >> 18) & 63) != 0) {
      ++blended;
      want = 0;
      for (unsigned shift = 0; shift < 18; shift += 6)
        want |= std::min<u32>((((p >> shift) & 63) * weight_top + ((three >> shift) & 63) * weight_3d) / 16, 63) << shift;
    }
    for (unsigned part = 0; part < 3; ++part)
      // Six bits as eight, the top two again at the bottom.
      if (((pixels[i] >> (part * 8)) & 255) != (((want >> (part * 6)) & 63) << 2 | ((want >> (part * 6)) & 63) >> 4)) {
        ++differ;
        break;
      }
  }
  std::printf("    %d of %zu pixels differ (%d with the 3D)\n", differ, screen.size(), blended);
  CHECK(differ == 0 && blended > 1000);
}

// The 2D pictures the graphics hardware draws (nds/frame_2d.h,
// shaders/engine2d.frag) against the processor's
// (nds::DisplayEngine::draw_line()): pixel for pixel, with the marks for
// the 3D scene. Video memory, palettes, sprites and registers at random,
// the registers and the palette changed from line to line.
namespace {

// How many colours the frame has, and how many pixels are marked for the
// scene.
struct Held {
  std::ptrdiff_t colours = 0, marked = 0;
  std::size_t blocks_changed = 0;  // Of video memory, while the lines were drawn.
};

Held random_2d_frame(bool second, std::mt19937& random, std::uint32_t version) {
  using bd::nds::DisplayEngine;
  VideoMemory video;
  // Every bank as plain memory, filled at random, then placed for the
  // engine: no page with two banks.
  for (unsigned b = 0; b < VideoMemory::kBanks; ++b)
    video.set_control(static_cast<VideoMemory::Bank>(b), VideoMemory::kEnable);
  for (u32 at = 0; at < VideoMemory::kAllSize; at += 4) video.write(0x06800000 + at, 4, static_cast<u32>(random()));
  const auto place = [&](VideoMemory::Bank bank, unsigned use, unsigned offset) {
    video.set_control(bank, static_cast<std::uint8_t>(VideoMemory::kEnable | use | offset << 3));
  };
  if (!second) {
    place(VideoMemory::kA, 1, 0);
    place(VideoMemory::kB, 1, 1);
    place(VideoMemory::kC, 1, 2);
    place(VideoMemory::kE, 2, 0);
    place(VideoMemory::kF, 4, 0);  // Extended palettes of backgrounds 0 and 1,
    if (random() % 2 != 0) place(VideoMemory::kG, 4, 1);  // of 2 and 3,
    else place(VideoMemory::kG, 5, 0);                   // or of the sprites.
  } else {
    place(VideoMemory::kC, 4, 0);
    place(VideoMemory::kD, 4, 0);
    place(VideoMemory::kH, 2, 0);
    place(VideoMemory::kI, 3, 0);
  }
  std::array<std::uint8_t, 0x400> palette{}, sprites{};
  for (auto& b : palette) b = static_cast<std::uint8_t>(random());
  for (auto& b : sprites) b = static_cast<std::uint8_t>(random());

  // One engine draws, the other records; both are given the same writes.
  const auto which = second ? DisplayEngine::kSub : DisplayEngine::kMain;
  bd::nds::Arm9Io io_drawn, io_recorded;
  DisplayEngine drawn(which, video, palette.data(), sprites.data()), recorded(which, video, palette.data(), sprites.data());
  drawn.attach(io_drawn);
  recorded.attach(io_recorded);
  drawn.set_hardware_3d(true);
  recorded.set_hardware_3d(true);
  const u32 base = DisplayEngine::kBases[which];
  const auto reg = [&](u32 offset, unsigned size, u32 value) {
    io_drawn.write(base + offset, size, value);
    io_recorded.write(base + offset, size, value);
  };
  const auto control = [&] {
    u32 c = static_cast<u32>(random());
    if (random() % 4 != 0) c = (c & ~(3u << 16)) | 1u << 16;  // Mostly the picture.
    reg(DisplayEngine::kControl, 4, c);
  };
  const auto point = [&] { return static_cast<u32>(static_cast<int>(random() % 0x40000) - 0x20000); };
  const auto some_registers = [&](bool all) {
    const auto maybe = [&] { return all || random() % 6 == 0; };
    if (maybe()) control();
    for (u32 n = 0; n < 4; ++n) {
      if (maybe()) reg(DisplayEngine::kBackgroundControl + 2 * n, 2, random());
      if (maybe()) reg(DisplayEngine::kScroll + 4 * n, 4, random());
    }
    for (u32 n = 0; n < 2; ++n) {
      const u32 at = DisplayEngine::kMatrix + 0x10 * n;
      if (maybe()) reg(at, 4, random());
      if (maybe()) reg(at + 4, 4, random());
      if (maybe()) reg(at + 8, 4, point());
      if (maybe()) reg(at + 12, 4, point());
    }
    if (maybe()) reg(DisplayEngine::kWindowAcross, 4, random());
    if (maybe()) reg(DisplayEngine::kWindowAcross + 4, 4, random() & 0xBFBFBFBF);
    if (maybe()) reg(DisplayEngine::kWindowInside, 4, random());
    if (maybe()) reg(DisplayEngine::kMosaic, 2, random());
    if (maybe()) reg(DisplayEngine::kBlendControl, 4, random());
    if (maybe()) reg(DisplayEngine::kBlendBrightness, 2, random());
    if (maybe()) reg(DisplayEngine::kMasterBrightness, 2, random() % 3 == 0 ? random() : 0);
  };

  bd::nds::Frame2D frame;
  frame.by_hardware = true;
  frame.second_engine = second;
  const auto copy = [](std::vector<std::uint8_t>& copies, const std::uint8_t* memory) {
    const std::size_t n = copies.size();
    if (n == 0 || !std::equal(memory, memory + 0x400, copies.end() - 0x400)) copies.insert(copies.end(), memory, memory + 0x400);
    return static_cast<u32>(copies.size() / 0x400 - 1);
  };
  some_registers(true);
  drawn.start_frame();
  recorded.start_frame();
  // Video memory as the frame begins, and as it changes while the lines are
  // drawn: runs of 256 bytes or fewer, where the engine reads.
  bd::nds::Video2DRecorder recorder;
  REQUIRE(recorder.place(video));
  recorder.begin(video, !second, second);
  const u32 shown_memory = second ? 0x06200000 : 0x06000000;
  for (int line = 0; line < kHeight; ++line) {
    drawn.start_line(line);
    recorded.start_line(line);
    if (line != 0 && random() % 8 == 0) some_registers(false);
    if (random() % 16 == 0) palette[random() % 0x400] = static_cast<std::uint8_t>(random());
    if (random() % 4 == 0) {
      const u32 at = shown_memory + (static_cast<u32>(random()) % 0x20000 & ~3u);
      for (u32 n = 0, words = 1 + random() % 64; n < words; ++n) video.write(at + 4 * n, 4, static_cast<u32>(random()));
    }
    recorder.line(video, static_cast<unsigned>(line));
    drawn.draw_line(line);
    const u32 p = copy(frame.palettes, palette.data()), s = copy(frame.sprites, sprites.data());
    recorded.record_line(line, frame, p, s);
  }

  REQUIRE(recorder.finish() == 0);
  // A new number for every page: the memory is new.
  bd::nds::Video2D video_2d = recorder.video_2d();
  video_2d.versions.assign(VideoMemory::kAllPages, version);

  // Drawn with an empty scene to put in, so that the marks stay.
  Hardware3D& h = hardware();
  Frame empty;
  static std::uint64_t last_serial = 0;
  const bd::nds::HardwareScene* scene = nullptr;
  do scene = &empty.r.build_scene(empty.frame, empty.video);
  while (scene->serial <= last_serial);
  last_serial = scene->serial;
  Hardware3D::Picture picture;
  picture.lines = &frame;
  picture.video = &video_2d;
  bool failed = false;
  REQUIRE(h.draw(0, picture, scene, 0, nullptr, 0, 1, failed) != nullptr);
  std::vector<u32> pixels;
  REQUIRE(h.read_2d(pixels));
  int differ = 0;
  const auto eight = [](u32 six) { return (six & 63) << 2 | (six & 63) >> 4; };
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const u32 c = drawn.screen()[i];
    const u32 wanted = eight(c) | eight(c >> 6) << 8 | eight(c >> 12) << 16 | (c >> 24) << 24;
    if (pixels[i] == wanted) continue;
    if (differ++ < 5)
      std::printf("    (%zu, %zu): %08x, not %08x (control %08x)\n", i % 256, i / 256, pixels[i], wanted,
                  frame.lines[i / 256 * bd::nds::Frame2D::kWords]);
  }
  std::vector<u32> colours(drawn.screen());
  std::sort(colours.begin(), colours.end());
  const auto marked = std::count_if(colours.begin(), colours.end(), [](u32 c) { return c >> 24 != 0; });
  const auto kinds = std::unique(colours.begin(), colours.end()) - colours.begin();
  const std::size_t blocks_changed = video_2d.changed.size() / bd::nds::Video2D::kBlockSize;
  std::printf("    %s engine: %d of %zu pixels differ (%td colours, %td pixels marked, %zu palettes, %zu blocks changed)\n",
              second ? "second" : "main", differ, pixels.size(), kinds, marked, frame.palettes.size() / 0x400,
              blocks_changed);
  CHECK(differ == 0);
  return {kinds, marked, blocks_changed};
}

}  // namespace

TEST(hardware_2d_pictures_as_the_processor_draws_them) {
  hardware();
  std::mt19937 random(2203);
  // That the frames have much in them: colours, marks for the scene, and
  // video memory changed while the lines were drawn.
  int full = 0, with_marks = 0, with_changes = 0;
  for (std::uint32_t n = 1; n <= 36; ++n) {
    const Held held = random_2d_frame(n % 3 == 0, random, n);
    full += held.colours > 100 ? 1 : 0;
    with_marks += held.marked != 0 ? 1 : 0;
    with_changes += held.blocks_changed >= 20 ? 1 : 0;
  }
  CHECK(full >= 30 && with_marks >= 3 && with_changes >= 30);
}

// Widescreen: the scene from the wide frame, drawn a third wider, with the
// DS's 256 columns in the middle (43 columns on each side at 1x): there the
// 2D picture with the scene put in where marked; at the sides the scene on
// the lines that show it, black on the others.
TEST(hardware_draws_a_wide_scene_with_the_screen_in_the_middle) {
  Hardware3D& h = hardware();
  CHECK(Hardware3D::picture_width(1, true) == 342 && Hardware3D::picture_width(3, true) == 1024 &&
        Hardware3D::picture_width(4, false) == 1024);
  Frame f;
  // Red with perspective from past the left edge to the middle of the DS's
  // screen, at w = 2 (so not flat).
  const u32 red = 63;
  f.polygon({Frame::vertex(-43, 0, red, 0x100000, 2 * kOne), Frame::vertex(-43, 192, red, 0x100000, 2 * kOne),
             Frame::vertex(128, 192, red, 0x100000, 2 * kOne), Frame::vertex(128, 0, red, 0x100000, 2 * kOne)},
            kOpaque | number(1));
  // The top half shows the 3D; the bottom half is a blue 2D layer.
  std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
  for (std::size_t i = screen.size() / 2; i < screen.size(); ++i) screen[i] = 63u << 12;
  h.set_wide(true);
  for (const int scale : {1, 3}) {
    static std::uint64_t last_serial = 0;
    const bd::nds::HardwareScene* scene = nullptr;
    do scene = &f.r.build_scene(f.frame, f.video, true);
    while (scene->serial <= last_serial);
    last_serial = scene->serial;
    CHECK(scene->wide);
    bool failed = false;
    REQUIRE(h.draw(0, screen, scene, 0, nullptr, 0, scale, failed) != nullptr && !failed);
    std::vector<u32> pixels;
    REQUIRE(h.read_screen(0, pixels));
    const int width = Hardware3D::picture_width(scale, true), left = (width - kWidth * scale) / 2;
    REQUIRE(pixels.size() == static_cast<std::size_t>(width) * kHeight * scale);
    const auto at = [&](int x, int y) { return pixels[static_cast<std::size_t>(y * scale) * width + x] & 0xFFFFFF; };
    // The DS's column 127 is red, 128 not: the screen is exactly the middle.
    CHECK(at(left + 127 * scale, 10) == 0x0000FF && at(left + 128 * scale, 10) == 0);
    // The left side shows the scene on the lines with 3D, black elsewhere.
    CHECK(at(0, 10) == 0x0000FF && at(left - 1, 10) == 0x0000FF);
    CHECK(at(0, 150) == 0 && at(left - 1, 150) == 0 && at(left, 150) == 0xFF0000);
    // The right side: past the red polygon, nothing.
    CHECK(at(width - 1, 10) == 0 && at(width - 1, 150) == 0);
  }
  // Where outlines are looked for: across all of a wide picture.
  const std::array<u32, 4> area = Hardware3D::outline_area(f.r.scene(), {0, 0, 256, 96}, 1, true);
  CHECK(area[0] == 0 && area[2] >= 170 && area[2] <= 173 && area[3] <= 98);
  h.set_wide(false);
}

// A picture made larger than the window shows it is made smaller by
// averaging: at a quarter, each pixel the mean of its 4 by 4.
TEST(hardware_downscales_a_picture_by_averaging) {
  Hardware3D& h = hardware();
  Frame f;
  f.control(PolygonRenderer::kEdgeMarking);
  cave(f);
  const std::vector<u32> large = f.graphics_hardware(4);
  SDL_Texture* small = h.downscaled(0, 0, 1024, 256, 192);
  REQUIRE(small != nullptr);
  std::vector<u32> pixels;
  REQUIRE(h.read_downscaled(0, pixels) && pixels.size() == static_cast<std::size_t>(kWidth) * kHeight);
  int differ = 0;
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x)
      for (unsigned part = 0; part < 3; ++part) {
        int sum = 0;
        for (int j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i)
            sum += static_cast<int>((large[static_cast<std::size_t>(y * 4 + j) * 1024 + x * 4 + i] >> (8 * part)) & 255);
        const int got = static_cast<int>((pixels[static_cast<std::size_t>(y) * kWidth + x] >> (8 * part)) & 255);
        if (std::abs(got - sum / 16) > 1) ++differ;
      }
  CHECK(differ == 0);
}
