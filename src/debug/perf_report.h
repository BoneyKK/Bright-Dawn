// The performance report (--perf-report FILE; launcher, Debug page): one
// JSON file written at exit, to paste to Claude or attach to an issue.
//
// It starts with a short text summary ("summary": lines), then: the system
// (OS, CPU, memory, build), the GPU and its driver (debug/gpu_info.h), the
// known GPU issues matched and the workarounds applied, the settings, and
// the timing: for each frame-log column (debug/frame_log.h) its mean,
// median, 95th and 99th percentile and maximum in ms and its share of the
// host's time; the 20 slowest frames; the spikes (runs of frames above 1.5
// times the median) with what changed as each began; GPU problems with the
// frame they happened on; and a one-line verdict.
//
// Privacy: no ROM data, pictures or game text; no user or host name, and
// no file path. What the app gives it (PerfReport::Section) is all it holds.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "debug/frame_log.h"

namespace bd::debug {

// What changed in a frame (PerfFrame::events): bits.
enum PerfEvent : std::uint32_t {
  kEventPathSwitch = 1u << 0,   // A screen's 2D or the 3D went between the graphics hardware and the processor.
  kEventNewScene = 1u << 1,     // 3D came or went on a screen, or the polygon count changed by half or more.
  kEventAtlasRefill = 1u << 2,  // New textures were sent to the GPU's atlas.
  kEventPipeline = 1u << 3,     // The GPU made pipelines or render targets.
  kEventCapture = 1u << 4,      // A display capture was made, or a picture written (--capture, the hotkey).
};
std::string event_names(std::uint32_t events);

// One frame, as the frame log has it.
struct PerfFrame {
  std::uint64_t frame = 0;
  double total_us = 0;
  std::array<float, frame_log::kColumns> us{};
  std::uint32_t polygons = 0, vertices = 0;
  bool shown = false;                  // A picture was presented.
  bool top_3d = false, bottom_3d = false, capture_shown = false;
  bool top_2d_gpu = false, bottom_2d_gpu = false, gpu_3d = false;  // The renderer path.
  std::uint32_t events = 0;            // PerfEvent bits; kEventPathSwitch and kEventNewScene are found by add().
};

struct ColumnStats {
  std::string name;  // "total" or a frame-log column without "_us".
  double mean_ms = 0, median_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  double share = 0;  // Of the host's time, 0 to 1.
};

struct Spike {
  std::uint64_t first = 0, frames = 0;  // The first frame and how many in the run.
  double peak_ms = 0, mean_ms = 0;
  std::uint32_t events = 0;  // What changed as it began: the first frame's events.
  std::string costliest;     // The column that grew most over its median in the run.
};

class PerfReport {
 public:
  // A field of a section: text, or a number or flag written as is.
  struct Field {
    std::string key, value;
    bool text = true;
  };
  struct Section {
    std::string name;
    std::vector<Field> fields;
    void text(std::string key, std::string value) { fields.push_back({std::move(key), std::move(value), true}); }
    void number(std::string key, double value);
    void flag(std::string key, bool value) { fields.push_back({std::move(key), value ? "true" : "false", false}); }
  };

  static constexpr std::size_t kMostFrames = 400'000;  // About 1 h 50 min; later frames are counted, not kept.

  // A frame; it finds the path switch and the new scene against the one before.
  void add(PerfFrame frame);
  // A GPU problem (device lost, out of memory, a pipeline or shader not
  // made) or a fallback, with the frame it happened on.
  void note(std::uint64_t frame, std::string kind, std::string text);

  std::size_t frames() const { return frames_.size(); }
  std::uint64_t frames_left_out() const { return left_out_; }
  const std::vector<PerfFrame>& all() const { return frames_; }

  // "total" first, then each column, in the log's order.
  std::vector<ColumnStats> column_stats() const;
  std::vector<PerfFrame> slowest(std::size_t n) const;
  std::vector<Spike> spikes() const;
  // `sync`: the frame log's sync mode (GPU passes waited for: their time is
  // the GPU's). `vsync`: the wait for a swapchain image is then the display's.
  std::string verdict(bool sync, bool vsync) const;

  // The report: the summary lines, then `sections` in order, then the
  // timing and the GPU problems.
  std::string json(const std::vector<Section>& sections, bool sync, bool vsync) const;

 private:
  struct Note {
    std::uint64_t frame;
    std::string kind, text;
  };
  std::vector<PerfFrame> frames_;
  std::vector<Note> notes_;
  std::uint64_t left_out_ = 0;
};

// The percentile `p` (0 to 1) of sorted `values`, by the nearest rank.
double percentile(const std::vector<double>& sorted, double p);

}  // namespace bd::debug
