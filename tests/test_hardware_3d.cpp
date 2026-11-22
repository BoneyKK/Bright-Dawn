// The graphics hardware's 3D drawing (app/hardware_3d.h) against the
// processor's (nds::PolygonRenderer) at the DS's resolution, where the two
// are to agree: the outlines and the anti-aliased edges. On whatever Vulkan
// device SDL finds; a software one, such as Mesa's lavapipe, will do. Without
// one the tests are skipped.
#include <SDL3/SDL.h>

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
  // it: eight bits a part. The screen is all 3D.
  std::vector<u32> graphics_hardware(int scale = 1) {
    Hardware3D& h = hardware();
    const std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight, bd::nds::DisplayEngine::kMark3D);
    // The graphics hardware draws a scene again only when its serial is
    // new, and each frame here has a renderer of its own.
    static std::uint64_t last_serial = 0;
    const bd::nds::HardwareScene* scene = nullptr;
    do scene = &r.build_scene(frame, video);
    while (scene->serial <= last_serial);
    last_serial = scene->serial;
    bool failed = false;
    REQUIRE(h.draw(0, screen, scene, 0, 0, scale, failed) != nullptr || !failed);
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
  bool failed = false;
  REQUIRE(h.draw(0, screen, nullptr, 77, 0, 1, failed) != nullptr);
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
