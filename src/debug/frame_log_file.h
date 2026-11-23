// The frame log's file (debug/frame_log.h): a CSV file of a row a frame,
// and at the end a summary: the average and the 95th percentile of each
// column, and the time column that costs the most.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace bd::frame_log {

class File {
 public:
  // Opens (makes) the file. It measures the clock first, for about 50 ms.
  explicit File(const std::filesystem::path& path);
  // Writes the summary and closes the file.
  ~File();
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool is_open() const { return out_ != nullptr; }
  // The clock's ticks a microsecond, and what one switch between columns
  // costs (the log's own cost in a frame is about this times its switches).
  double ticks_per_us() const { return ticks_per_us_; }
  double ns_per_switch() const { return ns_per_switch_; }
  bool started() const { return started_; }
  // The first lines, each after "# ", then the names of the columns. A name
  // that ends in "_us" is a time; "total_us" is the sum of the others.
  void start(const std::vector<std::string>& comment_lines, const std::vector<std::string>& columns);
  void row(const std::vector<double>& values);

 private:
  static constexpr int kBins = 2800;  // Each 1% wider than the last: from 0 to about 10^12.
  struct Column {
    std::string name;
    double sum = 0;
    std::vector<std::uint32_t> count;  // By bin.
    std::vector<double> bin_sum;
  };
  std::FILE* out_ = nullptr;
  bool started_ = false;
  double ticks_per_us_ = 1, ns_per_switch_ = 0;
  std::vector<Column> columns_;
  std::uint64_t rows_ = 0;
};

}  // namespace bd::frame_log
