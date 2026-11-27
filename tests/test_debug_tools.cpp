// The debug tools without the game: frame by frame (debug/frame_stepper.h),
// the performance report's numbers on a handmade frame log
// (debug/perf_report.h), driver versions read as each vendor packs them and
// the known GPU issues matched (debug/gpu_info.h).

#include <string>
#include <vector>

#include "debug/frame_stepper.h"
#include "debug/gpu_info.h"
#include "debug/perf_report.h"
#include "test.h"

namespace {

using bd::debug::DriverVersion;
using bd::debug::GpuDescription;
using bd::debug::GpuIssue;
using bd::debug::PerfFrame;
using bd::debug::PerfReport;
using bd::frame_log::FrameStepper;

// A machine that counts the frames it runs, held by the stepper as the game
// screen holds the console: after each frame it is asked, and while held
// it runs nothing; `redraws` window redraws happen while held.
struct Machine {
  FrameStepper stepper;
  std::uint64_t frames = 0;
  // Runs until `frames` reaches `until` or the stepper holds with nothing
  // to do for `redraws` redraws in a row.
  void run(std::uint64_t until, int redraws = 3) {
    while (frames < until) {
      int idle = 0;
      while (stepper.paused()) {
        if (stepper.held_redrawn()) break;
        if (++idle >= redraws) return;  // Held: the test steps or resumes.
      }
      ++frames;  // The frame runs.
      stepper.after_frame(frames);
    }
  }
};

bool near(double a, double b) { return a > b - 1e-6 && a < b + 1e-6; }

}  // namespace

TEST(stepper_pause_holds_the_machine_after_the_current_frame) {
  Machine m;
  m.run(5);
  CHECK(m.frames == 5);
  m.stepper.pause();
  m.run(100);
  // The frame being run when asked (the 6th) ends, then nothing runs.
  CHECK(m.frames == 6);
  CHECK(m.stepper.paused());
  m.run(100);
  CHECK(m.frames == 6);
}

TEST(stepper_step_runs_exactly_one_frame) {
  Machine m;
  m.stepper.pause();
  m.run(100);
  REQUIRE(m.frames == 1);
  for (std::uint64_t n = 2; n <= 5; ++n) {
    m.stepper.step();
    m.run(100);
    CHECK(m.frames == n);
    CHECK(m.stepper.paused());
  }
  // A step while running does nothing; resume runs on.
  m.stepper.resume();
  m.stepper.step();
  m.run(50);
  CHECK(m.frames == 50);
  CHECK(!m.stepper.paused());
}

TEST(stepper_scripted_pause_at_then_steps_then_runs_on) {
  Machine m;
  m.stepper.script(10, 3);
  // Held after 10; three steps one a redraw (11, 12, 13); then it runs on.
  m.run(40);
  CHECK(m.frames == 40);
  // Without steps: held at 10 until a hotkey.
  Machine held;
  held.stepper.script(10, 0);
  held.run(40);
  CHECK(held.frames == 10);
  held.stepper.resume();
  held.run(40);
  CHECK(held.frames == 40);
}

// The statistics on a handmade log: 100 frames, the total 1..100 ms, ARM9
// game code three quarters of it and the 2D pass a quarter.
TEST(perf_report_statistics_on_a_handmade_log) {
  PerfReport report;
  for (int i = 1; i <= 100; ++i) {
    PerfFrame f;
    f.frame = static_cast<std::uint64_t>(i);
    f.total_us = i * 1000.0;
    f.us[bd::frame_log::kArm9] = static_cast<float>(i * 750.0);
    f.us[bd::frame_log::kDisplayGpu] = static_cast<float>(i * 250.0);
    f.shown = true;
    report.add(f);
  }
  const auto stats = report.column_stats();
  REQUIRE(stats.size() == 1 + bd::frame_log::kColumns);
  CHECK(stats[0].name == "total");
  CHECK(near(stats[0].mean_ms, 50.5));
  CHECK(near(stats[0].median_ms, 50));  // Nearest rank: the 50th of 100.
  CHECK(near(stats[0].p95_ms, 95));
  CHECK(near(stats[0].p99_ms, 99));
  CHECK(near(stats[0].max_ms, 100));
  CHECK(near(stats[0].share, 1));
  const auto& arm9 = stats[1 + bd::frame_log::kArm9];
  CHECK(arm9.name == "arm9");
  CHECK(near(arm9.share, 0.75));
  CHECK(near(arm9.p99_ms, 74.25));
  CHECK(near(stats[1 + bd::frame_log::kDisplayGpu].share, 0.25));
  // The slowest first.
  const auto slow = report.slowest(20);
  REQUIRE(slow.size() == 20);
  CHECK(slow[0].frame == 100 && slow[19].frame == 81);
  // Spikes: above 1.5 times the median (75 ms), one run, 76 to 100.
  const auto runs = report.spikes();
  REQUIRE(runs.size() == 1);
  CHECK(runs[0].first == 76 && runs[0].frames == 25);
  CHECK(near(runs[0].peak_ms, 100));
  CHECK(runs[0].costliest == "arm9");
  // The verdict names the largest share and the worst p99; all on the CPU.
  const std::string v = report.verdict(true, false);
  CHECK(v.find("largest share: arm9 (75%") != std::string::npos);
  CHECK(v.find("worst p99: arm9") != std::string::npos);
  CHECK(v.find("on the CPU") != std::string::npos);
}

TEST(perf_report_finds_what_changed_as_a_spike_began) {
  PerfReport report;
  for (int i = 1; i <= 40; ++i) {
    PerfFrame f;
    f.frame = static_cast<std::uint64_t>(i);
    f.total_us = 10000;
    f.us[bd::frame_log::kArm9] = 10000;
    f.top_2d_gpu = f.bottom_2d_gpu = f.gpu_3d = true;
    f.polygons = 1000;
    f.top_3d = true;
    if (i >= 20 && i < 23) {
      // Three slow frames that begin as the bottom screen's 2D goes to the
      // processor and new textures are sent.
      f.total_us = 30000;
      f.us[bd::frame_log::kDisplayMain] = 20000;
      f.bottom_2d_gpu = false;
      if (i == 20) f.events = bd::debug::kEventAtlasRefill;
    }
    report.add(f);
  }
  const auto runs = report.spikes();
  REQUIRE(runs.size() == 1);
  CHECK(runs[0].first == 20 && runs[0].frames == 3);
  CHECK((runs[0].events & bd::debug::kEventPathSwitch) != 0);
  CHECK((runs[0].events & bd::debug::kEventAtlasRefill) != 0);
  CHECK(runs[0].costliest == "2d_main");
  // Waiting on the GPU: in sync mode its passes count as the GPU's.
  PerfReport gpu;
  for (int i = 1; i <= 10; ++i) {
    PerfFrame f;
    f.frame = static_cast<std::uint64_t>(i);
    f.total_us = 10000;
    f.us[bd::frame_log::kArm9] = 2000;
    f.us[bd::frame_log::kScenePass] = 8000;
    gpu.add(f);
  }
  CHECK(gpu.verdict(true, false).find("waiting on the GPU") != std::string::npos);
  CHECK(gpu.verdict(false, false).find("on the CPU") != std::string::npos);
  // The JSON holds the summary first and no path.
  const std::string json = gpu.json({}, true, false);
  CHECK(json.rfind("{\n  \"summary\": [", 0) == 0);
  CHECK(json.find("\"verdict\"") != std::string::npos);
}

// Driver versions as each vendor packs VkPhysicalDeviceProperties::driverVersion.
TEST(driver_version_decoded_by_vendor) {
  using namespace bd::debug;
  // NVIDIA 560.94 (Windows and Linux alike): 10.8.8.6 bits.
  const std::uint32_t nvidia = 560u << 22 | 94u << 14;
  CHECK(decode_driver_version(kVendorNvidia, kDriverNvidiaProprietary, nvidia, "windows").text() == "560.94.0.0");
  CHECK(decode_driver_version(kVendorNvidia, 0, 535u << 22 | 4u << 14 | 1u << 6 | 3u, "linux").text() == "535.04.1.3");
  // NVIDIA's GPU with Mesa's NVK: as Mesa numbers.
  CHECK(decode_driver_version(kVendorNvidia, kDriverMesaNvk, 24u << 22 | 2u << 12 | 5u, "linux").text() == "24.2.5");
  // Intel on Windows: 18.14 bits, 101.5444.
  const std::uint32_t intel = 101u << 14 | 5444u;
  CHECK(decode_driver_version(kVendorIntel, kDriverIntelWindows, intel, "windows").text() == "101.5444");
  CHECK(decode_driver_version(kVendorIntel, 0, intel, "windows").scheme == "intel-windows");
  // Intel with Mesa (ANV) on Linux: VK_MAKE_VERSION, Mesa 24.1.3.
  const std::uint32_t mesa = 24u << 22 | 1u << 12 | 3u;
  CHECK(decode_driver_version(kVendorIntel, kDriverIntelMesa, mesa, "linux").text() == "24.1.3");
  CHECK(decode_driver_version(kVendorIntel, 0, mesa, "linux").text() == "24.1.3");
  // AMD: its own driver and Mesa's RADV, both VK_MAKE_VERSION.
  CHECK(decode_driver_version(kVendorAmd, kDriverAmdProprietary, 2u << 22 | 0u << 12 | 279u, "windows").text() == "2.0.279");
  CHECK(decode_driver_version(kVendorAmd, kDriverMesaRadv, mesa, "linux").text() == "24.1.3");
  // Comparable: each part 16 bits, the first the highest.
  DriverVersion a = decode_driver_version(kVendorNvidia, 0, nvidia, "windows");
  DriverVersion b = decode_driver_version(kVendorNvidia, 0, 561u << 22, "windows");
  CHECK(a.comparable() < b.comparable());
  CHECK(api_version_text(1u << 22 | 3u << 12 | 280u) == "1.3.280");
}

TEST(gpu_issues_match_by_vendor_device_driver_os_and_version) {
  using namespace bd::debug;
  // The project's table starts empty.
  CHECK(known_gpu_issues().empty());
  const auto version = [](std::uint32_t major, std::uint32_t minor) {
    DriverVersion d;
    d.parts[0] = major;
    d.parts[1] = minor;
    d.count = 2;
    return d.comparable();
  };
  const GpuIssue table[] = {
      // Intel on Windows, devices 0x9A40..0x9A7F, drivers 101.4000 to 101.5000.
      {kVendorIntel, 0x9A40, 0x9A7F, "", "windows", version(101, 4000), version(101, 5000), "intel range",
       kWorkaroundProcessor2D},
      // Any NVIDIA from 560 on.
      {kVendorNvidia, 0, 0, "", "", version(560, 0), 0, "nvidia from 560", kWorkaroundVsync},
      // Mesa's RADV by driver name, any version.
      {kVendorAmd, 0, 0, "radv", "linux", 0, 0, "radv", kWorkaroundProcessor3D},
  };
  GpuDescription intel;
  intel.os = "windows";
  intel.vendor_id = kVendorIntel;
  intel.device_id = 0x9A49;
  intel.driver_id = kDriverIntelWindows;
  intel.driver_version = 101u << 14 | 4502u;
  auto found = match_gpu_issues(intel, table);
  REQUIRE(found.size() == 1);
  CHECK(found[0]->problem == "intel range");
  intel.driver_version = 101u << 14 | 5001u;  // Past the range.
  CHECK(match_gpu_issues(intel, table).empty());
  intel.driver_version = 101u << 14 | 5000u;  // Its end: in.
  CHECK(match_gpu_issues(intel, table).size() == 1);
  intel.device_id = 0x9A80;  // Another device.
  CHECK(match_gpu_issues(intel, table).empty());
  intel.device_id = 0x9A49;
  intel.os = "linux";  // Another OS.
  CHECK(match_gpu_issues(intel, table).empty());

  GpuDescription nvidia;
  nvidia.os = "linux";
  nvidia.vendor_id = kVendorNvidia;
  nvidia.driver_id = kDriverNvidiaProprietary;
  nvidia.driver_version = 559u << 22 | 99u << 14;
  CHECK(match_gpu_issues(nvidia, table).empty());
  nvidia.driver_version = 560u << 22 | 31u << 14;
  found = match_gpu_issues(nvidia, table);
  REQUIRE(found.size() == 1);
  CHECK(found[0]->workarounds == kWorkaroundVsync);

  GpuDescription amd;
  amd.os = "linux";
  amd.vendor_id = kVendorAmd;
  amd.driver_name = "RADV NAVI21";
  found = match_gpu_issues(amd, table);
  REQUIRE(found.size() == 1);
  CHECK(found[0]->problem == "radv");
  amd.driver_name = "AMD proprietary driver";
  CHECK(match_gpu_issues(amd, table).empty());
  CHECK(workaround_names(kWorkaroundProcessor2D | kWorkaroundVsync) == "2D by the processor, vsync on");
}
