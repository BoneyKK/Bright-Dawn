// The frame log (launcher, Debug page): what each frame costs, column by
// column, for bright-dawn-frames.csv (debug/frame_log_file.h).
//
// One column is "current" at any moment. A Scope makes its column current
// and, when it ends, gives the time back to the column that was current
// before, so each moment goes to exactly one column and the columns of a
// frame add up to its total. Time not in any Scope goes to the ARM9's game
// code, where the console spends it when nothing else is said.
//
// Off unless switched on (set_enabled): a Scope then does nothing but test
// a flag. Only the main thread keeps time; the state is per thread.
#pragma once

#include <chrono>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define BD_FRAME_LOG_TSC 1
#endif

namespace bd::frame_log {

// The times, in the order of the file's columns.
enum Column : int {
  kArm9,           // ARM9 game code: translated, and the memory, I/O and BIOS calls it makes.
  kInterpreter,    // ARM9 game code run by the interpreter.
  kArm7,           // The ARM7's work, done natively: IPC services, sound driver and sequences, sound mixing.
  kDma,            // DMA copies, but for those into the geometry engine.
  kOtherHardware,  // Time keeping: timers, video timing, interrupts, sleeping until an event.
  kGeometry,       // The geometry engine's commands, and the DMA copies that feed it.
  kBuildScene,     // PolygonRenderer::build_scene: the scene for the graphics hardware.
  kProcessor3D,    // PolygonRenderer::draw: the 3D picture drawn by the processor.
  kDisplayMain,    // 2D display engine A (main): its lines, or what they are drawn with (for the GPU).
  kDisplaySub,     // 2D display engine B (sub): the same.
  kCaptureCpu,     // Display captures by the processor; finding captures on the screens.
  kHost,           // Window events, input, sound out, the menu, the save.
  kUpload,         // Upload to the GPU: the transfer buffer filled, the copy pass.
  kScenePass,      // The 3D scene pass (the triangles).
  kRaster,         // The rasterizer at 1x (compute), when it runs.
  kDisplayGpu,     // The 2D pictures drawn by the GPU (shaders/engine2d.frag).
  kComposeTop,     // Compose of the top screen.
  kComposeBottom,  // Compose of the bottom screen.
  kCaptureGpu,     // Display captures by the GPU (their compose).
  kFilter,         // The screen filter passes: the SDL renderer enlarges and places both screens.
  kVsyncWait,      // Waiting for a swapchain image: vsync, or the GPU behind by the frames in flight.
  kPresent,        // SDL_RenderPresent.
  kPresentGpu,     // Sync mode only: the GPU's time for the filter passes and the copy to the window.
  kPaceWait,       // Waiting for the frame's time when vsync is off.
  kColumns
};

inline constexpr const char* kColumnNames[kColumns] = {
    "arm9_us",        "interpreter_us", "arm7_us",           "dma_us",         "other_hw_us",
    "geometry_us",    "build_scene_us", "processor_3d_us",   "2d_main_us",     "2d_sub_us",
    "capture_cpu_us", "host_us",        "upload_us",         "scene_pass_us",  "raster_1x_us",
    "2d_gpu_us",      "compose_top_us", "compose_bottom_us", "capture_gpu_us", "filter_us",
    "vsync_wait_us",  "present_us",     "present_gpu_us",    "pace_wait_us"};

// What else a frame's row has: counts and flags.
struct Counts {
  std::uint64_t polygons = 0, vertices = 0;  // The frame build_scene (or draw) was given.
  std::uint64_t gpu_vertices = 0, scene_draws = 0;  // The scene's vertices and draw calls, for the GPU.
  std::uint64_t scene_textures = 0, textures_decoded = 0, decoded_bytes = 0;
  std::uint64_t textures_uploaded = 0, texture_bytes = 0;  // New textures sent to the GPU's atlas.
  std::uint64_t screen_bytes = 0, vertex_bytes = 0, bank_bytes = 0, raster_bytes = 0;
  std::uint64_t video_bytes = 0;  // Video memory sent for the 2D pictures the GPU draws.
  std::uint64_t gpu_captures = 0, pictures = 0;
  std::uint64_t targets_made = 0;  // Times the GPU made its render targets anew (a new scale).
  // Of the scene last made: they stay from frame to frame.
  bool edge_marking = false, anti_aliasing = false, fog = false;
  bool capture_made = false, raster_ran = false;
};

inline std::uint64_t ticks() {
#if defined(BD_FRAME_LOG_TSC)
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

struct State {
  int current = kArm9;
  std::uint64_t since = 0;  // In ticks; 0 before the first switch.
  std::uint64_t spent[kColumns] = {};
  std::uint64_t switches = 0;
  Counts counts;
  bool sync = false;  // Each GPU pass is handed over and waited for.
};

inline bool& enabled_flag() {
  static bool on = false;
  return on;
}
inline bool enabled() { return enabled_flag(); }
inline void set_enabled(bool on) { enabled_flag() = on; }

inline State& state() {
  thread_local State s;
  return s;
}

// Makes `column` current; returns the column that was.
inline int switch_to(int column) {
  State& s = state();
  const std::uint64_t now = ticks();
  if (s.since != 0) s.spent[s.current] += now - s.since;
  s.since = now;
  ++s.switches;
  const int was = s.current;
  s.current = column;
  return was;
}

// The times from now on, from zero.
inline void restart(int column) {
  State& s = state();
  s.current = column;
  s.since = ticks();
  for (std::uint64_t& t : s.spent) t = 0;
  s.switches = 0;
  s.counts = Counts{};
}

class Scope {
 public:
  explicit Scope(int column) {
    // Nothing to do when the column is current already: the time goes to it
    // either way, and the clock is not read.
    if (enabled() && state().current != column) was_ = switch_to(column);
  }
  ~Scope() {
    if (was_ >= 0) switch_to(was_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  int was_ = -1;
};

}  // namespace bd::frame_log
