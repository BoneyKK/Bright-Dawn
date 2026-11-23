// The frame log (debug/frame_log.h): each moment goes to one column, and the
// file ends with the average and the 95th percentile of each column.
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "debug/frame_log.h"
#include "debug/frame_log_file.h"
#include "temp_dir.h"
#include "test.h"

namespace {

namespace fl = bd::frame_log;

void spin(std::chrono::microseconds how_long) {
  const auto began = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - began < how_long) {
  }
}

// The log is the program's one log: each test switches it off again.
struct Restore {
  ~Restore() {
    fl::set_enabled(false);
    fl::restart(fl::kArm9);
  }
};

std::uint64_t sum_of_spent() {
  std::uint64_t sum = 0;
  for (std::uint64_t t : fl::state().spent) sum += t;
  return sum;
}

}  // namespace

TEST(frame_log_scopes_do_nothing_when_it_is_off) {
  Restore restore;
  fl::set_enabled(false);
  fl::restart(fl::kArm9);
  {
    fl::Scope geometry(fl::kGeometry);
    spin(std::chrono::microseconds(200));
  }
  CHECK(fl::state().switches == 0);
  CHECK(fl::state().current == fl::kArm9);
}

TEST(frame_log_gives_each_moment_to_one_column) {
  Restore restore;
  fl::set_enabled(true);
  const std::uint64_t began = fl::ticks();
  fl::restart(fl::kArm9);
  {
    fl::Scope geometry(fl::kGeometry);
    spin(std::chrono::microseconds(500));
    {
      fl::Scope arm7(fl::kArm7);
      spin(std::chrono::microseconds(500));
    }
    // The same column again: nothing is switched.
    const std::uint64_t switches = fl::state().switches;
    {
      fl::Scope again(fl::kGeometry);
      spin(std::chrono::microseconds(100));
    }
    CHECK(fl::state().switches == switches);
    CHECK(fl::state().current == fl::kGeometry);
  }
  CHECK(fl::state().current == fl::kArm9);
  fl::switch_to(fl::kArm9);  // The times up to now.
  const std::uint64_t ended = fl::ticks();
  const fl::State& s = fl::state();
  CHECK(s.spent[fl::kGeometry] > 0 && s.spent[fl::kArm7] > 0);
  CHECK(s.spent[fl::kGeometry] > s.spent[fl::kArm7]);  // 600 us against 500 us.
  CHECK(s.spent[fl::kHost] == 0);
  // The columns add up to the time between the first switch and the last.
  CHECK(sum_of_spent() <= ended - began);
  CHECK(sum_of_spent() * 10 >= (ended - began) * 9);
}

TEST(frame_log_file_has_rows_and_a_summary) {
  Restore restore;
  bd_test::TempDir dir;
  const auto path = dir.path() / "frames.csv";
  {
    fl::File file(path);
    CHECK(file.is_open());
    CHECK(file.ticks_per_us() > 0);
    file.start({"first line"}, {"frame", "total_us", "a_us", "b_us", "count"});
    // 100 frames: a is 10 us in 96 of them and 1000 us in 4; b is 20 us.
    for (int i = 1; i <= 100; ++i) {
      const double a = i % 25 == 0 ? 1000 : 10;
      file.row({static_cast<double>(i), a + 20, a, 20, 3});
    }
  }
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  const std::string all = text.str();
  CHECK(all.rfind("# first line\nframe,total_us,a_us,b_us,count\n1,30,10,20,3\n", 0) == 0);
  CHECK(all.find("\n100,1020,1000,20,3\n") != std::string::npos);
  CHECK(all.find("# summary of 100 frames") != std::string::npos);
  // Averages: a (96 * 10 + 4 * 1000) / 100 = 49.6. The 95th percentile of a
  // is the 95th value from the smallest: 10.
  CHECK(all.find("\naverage,69.6,49.6,20.0,3.0\n") != std::string::npos);
  CHECK(all.find("\np95,30.0,10.0,20.0,3.0\n") != std::string::npos);
  CHECK(all.find("# costliest column: a_us, average 49.6 us of 69.6 us a frame (71.3%); p95 10.0 us\n") !=
        std::string::npos);
}
