// bd_scene_check: a 3D scene dumped from the game (bright-dawn --dump-3d)
// drawn by the processor and by the graphics hardware, compared, at 1x.
//
//   bd_scene_check DUMP [--out DIR] [--scale N] [--time N]
//
// Says how many pixels of the rasterizer's picture (shaders/raster.comp,
// finish.comp) differ from the processor's and, if any do, the first
// polygon in the order drawn after which they do. For each screen showing
// the scene, it compares the screen as the graphics hardware puts it
// together with the processor's picture put in the same way, apart for the
// pixels taken from the rasterizer and the rest. With --out, writes the
// pictures (PPM) there. With --scale, draws at that scale too (pictures
// only). With --time, draws the scene at that scale and puts it into each
// screen showing it N times, a new scene each time, waiting for the
// graphics hardware after each pass (the frame log's sync mode), and says
// the mean time of the scene pass and of putting the scene in (the frame
// log's compose columns, outlines included). Needs a Vulkan device.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "app/hardware_3d.h"
#include "core/log.h"
#include "debug/frame_log.h"
#include "nds/arm9_io.h"
#include "nds/polygon_renderer.h"
#include "nds/scene_dump.h"
#include "nds/video_memory.h"

namespace {

using bd::nds::GeometryEngine;
using bd::nds::PolygonRenderer;
using u32 = std::uint32_t;
constexpr int kWidth = 256, kHeight = 192;

int eight(u32 six) { return static_cast<int>(((six & 63) * 255 + 31) / 63); }

void write_ppm(const std::filesystem::path& path, const std::vector<u32>& pixels, int width, int height, bool six_bits) {
  FILE* out = std::fopen(path.string().c_str(), "wb");
  if (out == nullptr) return;
  std::fprintf(out, "P6 %d %d 255\n", width, height);
  for (int i = 0; i < width * height; ++i)
    for (unsigned k = 0; k < 3; ++k) {
      const u32 p = pixels[static_cast<std::size_t>(i)];
      std::fputc(six_bits ? eight(p >> (k * 6)) : static_cast<int>((p >> (k * 8)) & 255), out);
    }
  std::fclose(out);
}

struct Replay {
  bd::nds::SceneDump dump;
  bd::nds::Arm9Io io;
  PolygonRenderer r;
  bd::nds::VideoMemory video;
  bd::app::Hardware3D hardware;

  void prepare() {
    r.attach(io);
    bd::nds::restore_scene_inputs(dump.inputs, io, video);
    r.set_edge_marking(dump.edge_marking);
  }
  // The rasterizer's picture of the polygons given, at 1x, and the
  // processor's: how many pixels differ.
  int differ(const std::vector<GeometryEngine::Polygon>& polygons, std::vector<u32>* drawn = nullptr,
             std::vector<u32>* wanted = nullptr) {
    GeometryEngine::Frame frame = dump.inputs.frame;
    frame.polygons = polygons;
    r.set_fine_positions(false);
    const std::vector<u32> processor = r.draw(frame, video);
    const bd::nds::HardwareScene& scene = r.build_scene(frame, video);
    const std::vector<u32> screen(static_cast<std::size_t>(kWidth) * kHeight, 0x80000000u);
    bool failed = false;
    hardware.draw(0, screen, &scene, 0, 0, 1, failed);
    std::vector<u32> raster;
    if (failed || !hardware.read_raster(raster)) return -1;
    int count = 0;
    for (std::size_t i = 0; i < raster.size(); ++i) count += (raster[i] & 0xFFFFFF) != processor[i] ? 1 : 0;
    if (drawn != nullptr) *drawn = raster;
    if (wanted != nullptr) *wanted = processor;
    return count;
  }
};

void describe(const bd::nds::SceneDump& dump, std::size_t index) {
  const GeometryEngine::Polygon& p = dump.inputs.frame.polygons[index];
  std::printf("polygon %zu: attributes %08x, texture %08x, palette %x, %u corners, clockwise %d, strip order %d, flat %d\n",
              index, p.attributes, p.texture, p.palette, p.size, p.clockwise ? 1 : 0, p.strip_order ? 1 : 0, p.flat ? 1 : 0);
  for (unsigned i = 0; i < p.size; ++i) {
    const GeometryEngine::Vertex& v = dump.inputs.frame.vertices[p.first + std::size_t{i}];
    std::printf("  (%d, %d) fine (%d, %d) z %08x w %08x s %d t %d colour %05x\n", v.x, v.y, v.fine_x, v.fine_y,
                static_cast<u32>(v.z), static_cast<u32>(v.w), v.s, v.t, v.colour);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: bd_scene_check DUMP [--out DIR] [--scale N] [--time N]\n");
    return 2;
  }
  std::filesystem::path out;
  int scale = 1, times = 0;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string arg = argv[i];
    if (arg == "--out") out = argv[i + 1];
    else if (arg == "--time") times = std::max(std::atoi(argv[i + 1]), 0);
    else if (arg == "--scale") scale = std::clamp(std::atoi(argv[i + 1]), 1, bd::app::Hardware3D::kLargestScale);
  }
  bd::log::set_standard_error(true);  // What the graphics hardware notes, such as a full texture atlas.
  Replay replay;
  std::string error;
  if (!bd::nds::read_scene_dump(argv[1], replay.dump, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  replay.prepare();
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
  SDL_Renderer* renderer = SDL_Init(SDL_INIT_VIDEO) ? SDL_CreateGPURenderer(nullptr, nullptr) : nullptr;
  if (renderer == nullptr || !replay.hardware.open(renderer, error)) {
    std::fprintf(stderr, "error: no graphics device: %s%s\n", SDL_GetError(), error.c_str());
    return 1;
  }
  const bd::nds::SceneDump& dump = replay.dump;
  const auto& polygons = dump.inputs.frame.polygons;
  std::printf("%zu polygons, %zu vertices; control %04x; outlines %d; made at %dx\n", polygons.size(),
              dump.inputs.frame.vertices.size(), dump.inputs.control, static_cast<int>(dump.edge_marking), dump.scale);

  {
    replay.r.set_fine_positions(false);
    const bd::nds::HardwareScene& scene = replay.r.build_scene(dump.inputs.frame, replay.video);
    std::size_t texels = 0;
    for (const auto& t : scene.textures) texels += static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height);
    std::printf("%zu textures, %zu texels (the atlas holds %d)\n", scene.textures.size(), texels, 2048 * 2048);
  }
  // The rasterizer against the processor.
  std::vector<u32> raster, processor;
  const int differ = replay.differ(polygons, &raster, &processor);
  if (differ < 0) {
    std::printf("the rasterizer did not draw (the frame asks for neither outlines nor anti-aliasing)\n");
  } else {
    std::printf("rasterizer: %d pixels differ from the processor's\n", differ);
    int shown = 0;
    for (std::size_t i = 0; i < raster.size() && shown < 10; ++i)
      if ((raster[i] & 0xFFFFFF) != processor[i])
        std::printf("  (%zu, %zu) is %06x, not %06x\n", i % kWidth, i / kWidth, raster[i] & 0xFFFFFF, processor[i]), ++shown;
    if (differ > 0) {
      // The fewest polygons from the first that differ.
      std::size_t low = 0, high = polygons.size();
      while (low + 1 < high) {
        const std::size_t middle = (low + high) / 2;
        const std::vector<GeometryEngine::Polygon> first(polygons.begin(), polygons.begin() + static_cast<long>(middle));
        if (replay.differ(first) > 0) high = middle;
        else low = middle;
      }
      std::printf("the pixels differ once polygon %zu is drawn (and not before):\n", high - 1);
      describe(dump, high - 1);
      std::vector<u32> drawn, wanted;
      const std::vector<GeometryEngine::Polygon> upto(polygons.begin(), polygons.begin() + static_cast<long>(high));
      replay.differ(upto, &drawn, &wanted);
      int listed = 0;
      for (std::size_t i = 0; i < drawn.size() && listed < 10; ++i)
        if ((drawn[i] & 0xFFFFFF) != wanted[i])
          std::printf("  (%zu, %zu) is %06x, not %06x\n", i % kWidth, i / kWidth, drawn[i] & 0xFFFFFF, wanted[i]), ++listed;
      const std::vector<GeometryEngine::Polygon> alone{polygons[high - 1]};
      std::printf("drawn alone, %d pixels differ\n", replay.differ(alone));
    }
    if (!out.empty()) {
      write_ppm(out / "processor.ppm", processor, kWidth, kHeight, true);
      write_ppm(out / "rasterizer.ppm", raster, kWidth, kHeight, true);
    }
  }

  // Each screen showing the scene, as the graphics hardware puts it
  // together, against the processor's picture put in the same way.
  replay.differ(polygons);  // The whole scene again.
  PolygonRenderer& r = replay.r;
  for (int which = 0; which < 2; ++which) {
    const std::vector<u32>& screen = dump.screens[static_cast<std::size_t>(which)];
    if (dump.fills[static_cast<std::size_t>(which)] != 1 || screen.size() != static_cast<std::size_t>(kWidth) * kHeight) continue;
    bool failed = false;
    const bd::nds::HardwareScene& scene = r.build_scene(dump.inputs.frame, replay.video);
    replay.hardware.draw(which, screen, &scene, 0, dump.brightness[static_cast<std::size_t>(which)], 1, failed);
    std::vector<u32> shown_pixels, taken;
    if (failed || !replay.hardware.read_screen(which, shown_pixels)) continue;
    const bool have_taken = replay.hardware.read_raster(taken);
    int black[2] = {}, far[2] = {}, counted[2] = {};
    std::vector<u32> expected(shown_pixels.size());
    for (std::size_t i = 0; i < shown_pixels.size(); ++i) {
      const u32 s = screen[i];
      const u32 p = processor[i];
      int want[3];
      for (unsigned k = 0; k < 3; ++k) want[k] = eight(s >> (k * 6));
      // As compose.frag puts a pixel in (without the master brightness).
      if ((s >> 31) != 0 && (p >> 18) != 0) {
        const unsigned effect = (s >> 24) & 3, factor = (s >> 26) & 31;
        for (unsigned k = 0; k < 3; ++k) {
          double c = ((p >> (k * 6)) & 63) / 63.0;
          if (effect == 2) c += (1 - c) * factor / 16.0;
          if (effect == 3) c -= c * factor / 16.0;
          if (effect == 1) c = (eight(s >> (k * 6)) / 255.0) * (1 - (p >> 18) / 63.0) + c * ((p >> 18) / 63.0);
          want[k] = static_cast<int>(std::clamp(c, 0.0, 1.0) * 255 + 0.5);
        }
      }
      expected[i] = static_cast<u32>(want[0] | want[1] << 8 | want[2] << 16);
      const int group = have_taken && (taken[i] >> 24 & 1) != 0 ? 1 : 0;
      ++counted[group];
      const u32 got = shown_pixels[i];
      int most = 0;
      for (unsigned k = 0; k < 3; ++k) most = std::max(most, std::abs(want[k] - static_cast<int>((got >> (k * 8)) & 255)));
      if ((got & 0xFFFFFF) == 0 && (want[0] | want[1] | want[2]) != 0) ++black[group];
      if (most > 24) ++far[group];
    }
    std::printf("%s screen: %d pixels from the triangles (%d black where the processor's are not, %d off by more than 24), "
                "%d from the rasterizer (%d black, %d off)\n",
                which == 0 ? "top" : "bottom", counted[0], black[0], far[0], counted[1], black[1], far[1]);
    if (!out.empty()) {
      const std::string name = which == 0 ? "top" : "bottom";
      write_ppm(out / (name + "_hardware.ppm"), shown_pixels, kWidth, kHeight, false);
      write_ppm(out / (name + "_processor.ppm"), expected, kWidth, kHeight, false);
    }
    if (scale > 1 && !out.empty()) {
      r.set_fine_positions(dump.fine_positions);
      const bd::nds::HardwareScene& large = r.build_scene(dump.inputs.frame, replay.video);
      replay.hardware.draw(which, screen, &large, 0, dump.brightness[static_cast<std::size_t>(which)], scale, failed);
      if (!failed && replay.hardware.read_screen(which, shown_pixels))
        write_ppm(out / ((which == 0 ? "top_hardware_" : "bottom_hardware_") + std::to_string(scale) + "x.ppm"), shown_pixels,
                  kWidth * scale, kHeight * scale, false);
      r.set_fine_positions(false);
    }
  }
  if (times > 0) {
    // The frame log's clock, in ticks a microsecond.
    const auto began = std::chrono::steady_clock::now();
    const std::uint64_t ticks_began = bd::frame_log::ticks();
    while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(50)) {
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
    const double ticks_per_us = static_cast<double>(bd::frame_log::ticks() - ticks_began) / us;
    bd::frame_log::set_enabled(true);
    bd::frame_log::state().sync = true;
    r.set_fine_positions(dump.fine_positions);
    for (int which = 0; which < 2; ++which) {
      const std::vector<u32>& screen = dump.screens[static_cast<std::size_t>(which)];
      if (dump.fills[static_cast<std::size_t>(which)] != 1 || screen.size() != static_cast<std::size_t>(kWidth) * kHeight) continue;
      const int compose = which == 0 ? bd::frame_log::kComposeTop : bd::frame_log::kComposeBottom;
      double scene_us = 0, compose_us = 0;
      for (int n = 0; n <= times; ++n) {
        const bd::nds::HardwareScene& scene = r.build_scene(dump.inputs.frame, replay.video);
        bd::frame_log::restart(bd::frame_log::kHost);
        bool failed = false;
        replay.hardware.draw(which, screen, &scene, 0, dump.brightness[static_cast<std::size_t>(which)], scale, failed);
        bd::frame_log::switch_to(bd::frame_log::kHost);
        if (failed) break;
        if (n == 0) continue;  // The first is the textures' upload and the targets' making.
        scene_us += static_cast<double>(bd::frame_log::state().spent[bd::frame_log::kScenePass]) / ticks_per_us;
        compose_us += static_cast<double>(bd::frame_log::state().spent[compose]) / ticks_per_us;
      }
      std::printf("%s screen at %dx, %d times: scene pass %.0f us, compose %.0f us\n", which == 0 ? "top" : "bottom", scale,
                  times, scene_us / times, compose_us / times);
    }
  }
  return 0;
}
