#include "debug/perf_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>

namespace bd::debug {

namespace {

using frame_log::kColumnNames;
using frame_log::kColumns;

std::string column_name(int c) {
  std::string n = kColumnNames[c];
  if (n.size() > 3 && n.compare(n.size() - 3, 3, "_us") == 0) n.resize(n.size() - 3);
  return n;
}

std::string escaped(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char code[8];
          std::snprintf(code, sizeof code, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += code;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string json_text(const std::string& s) { return "\"" + escaped(s) + "\""; }

std::string number_text(double v) {
  if (!std::isfinite(v)) return "null";
  char text[40];
  if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(text, sizeof text, "%.0f", v);
  else std::snprintf(text, sizeof text, "%.3f", v);
  return text;
}

// The columns whose time is the GPU's: waited for at present, or, in sync
// mode, each pass (debug/frame_log.h).
bool gpu_column(int c, bool sync, bool vsync) {
  using namespace frame_log;
  if (c == kPresentGpu) return true;
  if (c == kVsyncWait) return !vsync;
  if (!sync) return false;
  return c == kScenePass || c == kRaster || c == kDisplayGpu || c == kComposeTop || c == kComposeBottom ||
         c == kCaptureGpu || c == kFilter;
}

// Time spent waiting for the frame's time to come: not a cost.
bool pacing_column(int c, bool vsync) { return c == frame_log::kPaceWait || (vsync && c == frame_log::kVsyncWait); }

std::string path_text(const PerfFrame& f) {
  return std::string("top 2D ") + (f.top_2d_gpu ? "GPU" : "processor") + ", bottom 2D " +
         (f.bottom_2d_gpu ? "GPU" : "processor") + ", 3D " + (f.gpu_3d ? "GPU" : "processor");
}

std::string screens_text(const PerfFrame& f) {
  std::string s = f.top_3d && f.bottom_3d ? "3D on both screens"
                  : f.top_3d               ? "3D on the top screen"
                  : f.bottom_3d            ? "3D on the bottom screen"
                                           : "2D only";
  if (f.capture_shown) s += ", a capture shown";
  if (!f.shown) s += ", not shown";
  return s;
}

}  // namespace

std::string event_names(std::uint32_t e) {
  std::string names;
  const auto add = [&](std::uint32_t bit, const char* name) {
    if ((e & bit) == 0) return;
    if (!names.empty()) names += ", ";
    names += name;
  };
  add(kEventPathSwitch, "path switch");
  add(kEventNewScene, "new scene");
  add(kEventAtlasRefill, "atlas refill");
  add(kEventPipeline, "pipeline creation");
  add(kEventCapture, "capture");
  return names.empty() ? "nothing seen" : names;
}

void PerfReport::Section::number(std::string key, double value) {
  fields.push_back({std::move(key), number_text(value), false});
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  // Nearest rank: the smallest value with at least p of the values at or below it.
  const double rank = std::ceil(p * static_cast<double>(sorted.size()));
  const std::size_t i = rank < 1 ? 0 : std::min(sorted.size() - 1, static_cast<std::size_t>(rank) - 1);
  return sorted[i];
}

void PerfReport::add(PerfFrame f) {
  if (frames_.size() >= kMostFrames) {
    ++left_out_;
    return;
  }
  if (!frames_.empty()) {
    const PerfFrame& b = frames_.back();
    if (b.top_2d_gpu != f.top_2d_gpu || b.bottom_2d_gpu != f.bottom_2d_gpu || b.gpu_3d != f.gpu_3d)
      f.events |= kEventPathSwitch;
    const bool count_jump = (b.polygons == 0) != (f.polygons == 0) ||
                            (b.polygons != 0 && (f.polygons * 2 < b.polygons || f.polygons > b.polygons * 2));
    if (b.top_3d != f.top_3d || b.bottom_3d != f.bottom_3d || count_jump) f.events |= kEventNewScene;
  }
  frames_.push_back(f);
}

void PerfReport::note(std::uint64_t frame, std::string kind, std::string text) {
  if (notes_.size() < 200) notes_.push_back({frame, std::move(kind), std::move(text)});
}

std::vector<ColumnStats> PerfReport::column_stats() const {
  std::vector<ColumnStats> stats;
  if (frames_.empty()) return stats;
  double all = 0;
  for (const PerfFrame& f : frames_) all += f.total_us;
  std::vector<double> v(frames_.size());
  for (int c = -1; c < kColumns; ++c) {
    for (std::size_t i = 0; i < frames_.size(); ++i) v[i] = c < 0 ? frames_[i].total_us : frames_[i].us[c];
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    std::sort(v.begin(), v.end());
    ColumnStats s;
    s.name = c < 0 ? "total" : column_name(c);
    s.mean_ms = sum / static_cast<double>(v.size()) / 1000;
    s.median_ms = percentile(v, 0.5) / 1000;
    s.p95_ms = percentile(v, 0.95) / 1000;
    s.p99_ms = percentile(v, 0.99) / 1000;
    s.max_ms = v.back() / 1000;
    s.share = all > 0 ? sum / all : 0;
    stats.push_back(s);
  }
  return stats;
}

std::vector<PerfFrame> PerfReport::slowest(std::size_t n) const {
  std::vector<PerfFrame> sorted = frames_;
  const std::size_t k = std::min(n, sorted.size());
  std::partial_sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end(),
                    [](const PerfFrame& a, const PerfFrame& b) { return a.total_us > b.total_us; });
  sorted.resize(k);
  return sorted;
}

std::vector<Spike> PerfReport::spikes() const {
  std::vector<Spike> found;
  if (frames_.size() < 2) return found;
  std::vector<double> totals(frames_.size());
  for (std::size_t i = 0; i < frames_.size(); ++i) totals[i] = frames_[i].total_us;
  std::sort(totals.begin(), totals.end());
  const double median = percentile(totals, 0.5);
  const double limit = 1.5 * median;
  std::array<double, kColumns> column_median{};
  for (int c = 0; c < kColumns; ++c) {
    std::vector<double> v(frames_.size());
    for (std::size_t i = 0; i < frames_.size(); ++i) v[i] = frames_[i].us[c];
    std::sort(v.begin(), v.end());
    column_median[c] = percentile(v, 0.5);
  }
  for (std::size_t i = 0; i < frames_.size();) {
    if (frames_[i].total_us <= limit) {
      ++i;
      continue;
    }
    Spike s;
    s.first = frames_[i].frame;
    s.events = frames_[i].events;
    std::array<double, kColumns> sum{};
    double total = 0;
    std::size_t j = i;
    for (; j < frames_.size() && frames_[j].total_us > limit; ++j) {
      s.peak_ms = std::max(s.peak_ms, frames_[j].total_us / 1000);
      total += frames_[j].total_us;
      for (int c = 0; c < kColumns; ++c) sum[c] += frames_[j].us[c];
    }
    s.frames = j - i;
    s.mean_ms = total / static_cast<double>(s.frames) / 1000;
    int most = 0;
    double grew = -1;
    for (int c = 0; c < kColumns; ++c) {
      const double g = sum[c] / static_cast<double>(s.frames) - column_median[c];
      if (g > grew) {
        grew = g;
        most = c;
      }
    }
    s.costliest = column_name(most);
    found.push_back(s);
    i = j;
  }
  return found;
}

std::string PerfReport::verdict(bool sync, bool vsync) const {
  const std::vector<ColumnStats> stats = column_stats();
  if (stats.size() < 2) return "no frames were recorded";
  int largest = 1, worst = 1;
  for (int c = 1; c <= kColumns; ++c) {
    if (pacing_column(c - 1, vsync)) continue;
    if (stats[c].share > stats[largest].share || pacing_column(largest - 1, vsync)) largest = c;
    if (stats[c].p99_ms > stats[worst].p99_ms || pacing_column(worst - 1, vsync)) worst = c;
  }
  double gpu = 0, busy = 0;
  for (int c = 0; c < kColumns; ++c) {
    if (pacing_column(c, vsync)) continue;
    busy += stats[c + 1].share;
    if (gpu_column(c, sync, vsync)) gpu += stats[c + 1].share;
  }
  const double gpu_part = busy > 0 ? gpu / busy : 0;
  char text[512];
  std::snprintf(text, sizeof text,
                "largest share: %s (%.0f%% of the host's time); worst p99: %s (%.2f ms); the time is %s "
                "(%.0f%% of the busy time waits on the GPU)%s",
                stats[largest].name.c_str(), stats[largest].share * 100, stats[worst].name.c_str(), stats[worst].p99_ms,
                gpu_part > 0.5 ? "waiting on the GPU" : "on the CPU", gpu_part * 100,
                sync ? "" : "; normal mode: GPU passes are not waited for, so their GPU time shows only at present");
  return text;
}

std::string PerfReport::json(const std::vector<Section>& sections, bool sync, bool vsync) const {
  const std::vector<ColumnStats> stats = column_stats();
  const std::vector<PerfFrame> slow = slowest(20);
  const std::vector<Spike> runs = spikes();
  std::string j = "{\n";
  // The summary first: lines to read before the numbers.
  std::vector<std::string> summary;
  summary.push_back("Bright Dawn performance report");
  for (const Section& s : sections)
    for (const Field& f : s.fields)
      if (f.key == "summary") summary.push_back(s.name + ": " + f.value);
  if (!stats.empty()) {
    char line[256];
    std::snprintf(line, sizeof line, "%zu frames; host time a frame: mean %.2f ms, median %.2f ms, p99 %.2f ms, max %.2f ms",
                  frames_.size(), stats[0].mean_ms, stats[0].median_ms, stats[0].p99_ms, stats[0].max_ms);
    summary.push_back(line);
    std::snprintf(line, sizeof line, "%zu spikes (runs of frames above 1.5 times the median)", runs.size());
    summary.push_back(line);
  }
  summary.push_back("verdict: " + verdict(sync, vsync));
  j += "  \"summary\": [\n";
  for (std::size_t i = 0; i < summary.size(); ++i)
    j += "    " + json_text(summary[i]) + (i + 1 < summary.size() ? ",\n" : "\n");
  j += "  ],\n";
  for (const Section& s : sections) {
    j += "  " + json_text(s.name) + ": {\n";
    std::size_t n = 0;
    for (const Field& f : s.fields) {
      if (f.key == "summary") continue;
      ++n;
    }
    std::size_t k = 0;
    for (const Field& f : s.fields) {
      if (f.key == "summary") continue;
      j += "    " + json_text(f.key) + ": " + (f.text ? json_text(f.value) : f.value) + (++k < n ? ",\n" : "\n");
    }
    j += "  },\n";
  }
  j += "  \"timing\": {\n";
  j += "    \"frames\": " + std::to_string(frames_.size()) + ",\n";
  j += "    \"frames_left_out\": " + std::to_string(left_out_) + ",\n";
  j += std::string("    \"mode\": ") + json_text(sync ? "sync: each GPU pass is waited for, so its time is the GPU's"
                                                   : "normal: CPU times; the GPU's show only where it is waited for") +
       ",\n";
  j += "    \"columns\": [\n";
  for (std::size_t i = 0; i < stats.size(); ++i) {
    const ColumnStats& s = stats[i];
    j += "      {\"name\": " + json_text(s.name) + ", \"mean_ms\": " + number_text(s.mean_ms) +
         ", \"median_ms\": " + number_text(s.median_ms) + ", \"p95_ms\": " + number_text(s.p95_ms) +
         ", \"p99_ms\": " + number_text(s.p99_ms) + ", \"max_ms\": " + number_text(s.max_ms) +
         ", \"share\": " + number_text(s.share) + "}" + (i + 1 < stats.size() ? ",\n" : "\n");
  }
  j += "    ],\n";
  j += "    \"slowest_frames\": [\n";
  for (std::size_t i = 0; i < slow.size(); ++i) {
    const PerfFrame& f = slow[i];
    // Its three costliest columns.
    std::array<int, kColumns> order;
    std::iota(order.begin(), order.end(), 0);
    std::partial_sort(order.begin(), order.begin() + 3, order.end(), [&](int a, int b) { return f.us[a] > f.us[b]; });
    std::string top;
    for (int k = 0; k < 3; ++k) {
      char part[96];
      std::snprintf(part, sizeof part, "%s%s %.2f ms", k == 0 ? "" : ", ", column_name(order[k]).c_str(), f.us[order[k]] / 1000.0);
      top += part;
    }
    j += "      {\"frame\": " + std::to_string(f.frame) + ", \"total_ms\": " + number_text(f.total_us / 1000) +
         ", \"costliest\": " + json_text(top) + ", \"screens\": " + json_text(screens_text(f)) +
         ", \"path\": " + json_text(path_text(f)) + ", \"polygons\": " + std::to_string(f.polygons) +
         ", \"vertices\": " + std::to_string(f.vertices) + ", \"events\": " + json_text(event_names(f.events)) + "}" +
         (i + 1 < slow.size() ? ",\n" : "\n");
  }
  j += "    ],\n";
  j += "    \"spikes\": [\n";
  const std::size_t shown_runs = std::min<std::size_t>(runs.size(), 100);
  for (std::size_t i = 0; i < shown_runs; ++i) {
    const Spike& s = runs[i];
    j += "      {\"first_frame\": " + std::to_string(s.first) + ", \"frames\": " + std::to_string(s.frames) +
         ", \"peak_ms\": " + number_text(s.peak_ms) + ", \"mean_ms\": " + number_text(s.mean_ms) +
         ", \"at_start\": " + json_text(event_names(s.events)) + ", \"grew_most\": " + json_text(s.costliest) + "}" +
         (i + 1 < shown_runs ? ",\n" : "\n");
  }
  j += "    ],\n";
  j += "    \"spikes_total\": " + std::to_string(runs.size()) + "\n";
  j += "  },\n";
  j += "  \"gpu_problems\": [\n";
  for (std::size_t i = 0; i < notes_.size(); ++i)
    j += "    {\"frame\": " + std::to_string(notes_[i].frame) + ", \"kind\": " + json_text(notes_[i].kind) +
         ", \"text\": " + json_text(notes_[i].text) + "}" + (i + 1 < notes_.size() ? ",\n" : "\n");
  j += "  ],\n";
  j += "  \"verdict\": " + json_text(verdict(sync, vsync)) + "\n";
  j += "}\n";
  return j;
}

}  // namespace bd::debug
