// What the performance report (debug/perf_report.h) says of the computer:
// the OS and its version, the processor, the memory, and the graphics card
// and its driver. No user or host name, and no path.
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "debug/gpu_info.h"
#include "debug/perf_report.h"

namespace bd::app {

// "linux", "windows", "macos" or SDL's name for the platform.
std::string os_id();
// The OS, its version, the processor's model, its cores and threads, the
// memory, and the build (type and commit).
debug::PerfReport::Section system_section();
// The graphics card SDL's GPU device draws with. On Vulkan, the vendor and
// device IDs, the device type, the driver's name, information and version,
// the API version and the device-local memory come from a Vulkan instance of
// its own (made and destroyed here; only when `ask_driver`); elsewhere, and
// without it, what SDL reports.
debug::GpuDescription describe_gpu(SDL_GPUDevice* gpu, bool ask_driver);

}  // namespace bd::app
