#include "debug/frame_log_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "debug/frame_log.h"

namespace bd::frame_log {

namespace {

int bin_of(double v) {
  if (!(v > 0)) return 0;
  return static_cast<int>(std::log1p(v) / std::log(1.01));
}

}  // namespace

File::File(const std::filesystem::path& path) {
#if defined(_WIN32)
  out_ = _wfopen(path.c_str(), L"wb");
#else
  out_ = std::fopen(path.c_str(), "wb");
#endif
  // The clock's ticks a microsecond, against the system's clock.
  const auto clock_began = std::chrono::steady_clock::now();
  const std::uint64_t ticks_began = ticks();
  while (std::chrono::steady_clock::now() - clock_began < std::chrono::milliseconds(50)) {
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - clock_began).count();
  ticks_per_us_ = static_cast<double>(ticks() - ticks_began) / us;
  if (!(ticks_per_us_ > 0)) ticks_per_us_ = 1;
  // What a switch costs: a Scope is two. The times this adds go when the
  // log restarts them.
  constexpr int kTimes = 100000;
  State& s = state();
  const int was = s.current;
  const std::uint64_t before = ticks();
  for (int i = 0; i < kTimes; ++i) {
    switch_to(kHost);
    switch_to(kArm9);
  }
  ns_per_switch_ = static_cast<double>(ticks() - before) / ticks_per_us_ * 1000.0 / (2.0 * kTimes);
  s.current = was;
}

void File::start(const std::vector<std::string>& comment_lines, const std::vector<std::string>& columns) {
  started_ = true;
  columns_.clear();
  for (const std::string& name : columns) {
    Column c;
    c.name = name;
    c.count.assign(kBins, 0);
    c.bin_sum.assign(kBins, 0);
    columns_.push_back(std::move(c));
  }
  if (out_ == nullptr) return;
  for (const std::string& line : comment_lines) std::fprintf(out_, "# %s\n", line.c_str());
  for (std::size_t i = 0; i < columns.size(); ++i) std::fprintf(out_, "%s%s", i == 0 ? "" : ",", columns[i].c_str());
  std::fputc('\n', out_);
}

void File::row(const std::vector<double>& values) {
  ++rows_;
  for (std::size_t i = 0; i < values.size() && i < columns_.size(); ++i) {
    Column& c = columns_[i];
    c.sum += values[i];
    const auto b = static_cast<std::size_t>(std::min(bin_of(values[i]), kBins - 1));
    ++c.count[b];
    c.bin_sum[b] += values[i];
  }
  if (out_ == nullptr) return;
  for (std::size_t i = 0; i < values.size(); ++i) std::fprintf(out_, "%s%.0f", i == 0 ? "" : ",", values[i]);
  std::fputc('\n', out_);
  // A second's rows at a time, so that little is lost if the program ends
  // without closing the file.
  if (rows_ % 60 == 0) std::fflush(out_);
}

File::~File() {
  if (out_ == nullptr) return;
  if (rows_ != 0 && !columns_.empty()) {
    std::vector<double> average(columns_.size()), p95(columns_.size());
    const auto wanted = static_cast<std::uint64_t>(std::ceil(0.95 * static_cast<double>(rows_)));
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      const Column& c = columns_[i];
      average[i] = c.sum / static_cast<double>(rows_);
      // The 95th percentile: the bin, from the smallest up, at which 95% of
      // the frames are counted (to 1%), as the average of what is in it.
      std::uint64_t seen = 0;
      for (std::size_t b = 0; b < c.count.size(); ++b) {
        seen += c.count[b];
        if (c.count[b] != 0 && seen >= wanted) {
          p95[i] = c.bin_sum[b] / c.count[b];
          break;
        }
      }
    }
    std::fprintf(out_, "\n# summary of %llu frames: the average, then the 95th percentile, of each column\n",
                 static_cast<unsigned long long>(rows_));
    for (int line = 0; line < 2; ++line) {
      const std::vector<double>& v = line == 0 ? average : p95;
      std::fprintf(out_, "%s", line == 0 ? "average" : "p95");
      for (std::size_t i = 1; i < v.size(); ++i) std::fprintf(out_, ",%.1f", v[i]);
      std::fputc('\n', out_);
    }
    // The time column that costs the most, on average.
    std::size_t most = columns_.size();
    double total = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      const std::string& n = columns_[i].name;
      if (n == "total_us") total = average[i];
      else if (n.size() > 3 && n.compare(n.size() - 3, 3, "_us") == 0 &&
               (most == columns_.size() || average[i] > average[most]))
        most = i;
    }
    if (most != columns_.size())
      std::fprintf(out_, "# costliest column: %s, average %.1f us of %.1f us a frame (%.1f%%); p95 %.1f us\n",
                   columns_[most].name.c_str(), average[most], total, total > 0 ? 100.0 * average[most] / total : 0.0,
                   p95[most]);
  }
  std::fclose(out_);
}

}  // namespace bd::frame_log
