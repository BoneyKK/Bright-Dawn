// The graphics card and its driver, as the performance report gives them
// (debug/perf_report.h), and the table of known GPU issues: driver versions
// decoded the way each vendor numbers them, and the issues matched against
// the running GPU at start.
//
// The table is data only: a vendor, a range of device IDs, a part of the
// driver's name, an OS, a range of driver versions, the problem and the
// workaround. It starts empty: an entry goes in only when a report shows
// the issue in this project (docs/DEVELOPMENT.md, Known GPU issues).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bd::debug {

// PCI vendor IDs.
inline constexpr std::uint32_t kVendorAmd = 0x1002, kVendorNvidia = 0x10DE, kVendorIntel = 0x8086,
                               kVendorApple = 0x106B, kVendorQualcomm = 0x5143, kVendorArm = 0x13B5,
                               kVendorMesa = 0x10005;  // VK_VENDOR_ID_MESA: llvmpipe and other software drivers.

// Vulkan's VkDriverId values this file uses.
inline constexpr std::uint32_t kDriverAmdProprietary = 1, kDriverAmdOpenSource = 2, kDriverMesaRadv = 3,
                               kDriverNvidiaProprietary = 4, kDriverIntelWindows = 5, kDriverIntelMesa = 6,
                               kDriverMesaLlvmpipe = 13, kDriverMoltenVk = 14, kDriverMesaTurnip = 18,
                               kDriverMesaNvk = 24;

struct GpuDescription {
  std::string backend;  // The SDL GPU backend in use: "vulkan", "direct3d12" or "metal"; empty for none.
  std::string os;       // "windows", "linux", "macos" or another.
  std::string name;
  std::uint32_t vendor_id = 0, device_id = 0;
  std::string device_type;  // "integrated", "discrete", "virtual", "cpu", "other"; empty when unknown.
  // Vulkan (VkPhysicalDeviceDriverProperties and VkPhysicalDeviceProperties);
  // zero and empty where not known.
  std::uint32_t driver_id = 0;
  std::string driver_name, driver_info;
  std::uint32_t driver_version = 0, api_version = 0;
  std::uint64_t vram_bytes = 0;  // Device-local memory; 0 where the API does not say.
  // What SDL reports (SDL_PROP_GPU_DEVICE_DRIVER_*), on any backend.
  std::string sdl_driver_name, sdl_driver_version, sdl_driver_info;
};

// A driver version as its vendor numbers it.
struct DriverVersion {
  std::uint32_t parts[4] = {0, 0, 0, 0};
  int count = 0;
  std::string scheme;  // How it was read: "nvidia", "intel-windows", "vulkan" (major.minor.patch, as Mesa and AMD).
  std::string text() const;
  // For ranges: the parts as one number, each part 16 bits, the first the highest.
  std::uint64_t comparable() const;
};

// VkPhysicalDeviceProperties::driverVersion, read as the vendor packs it:
// NVIDIA's own driver 10.8.8.6 bits; Intel's on Windows 18.14 bits; every
// other (Mesa's drivers, AMD's, MoltenVK) as VK_MAKE_VERSION, 10.10.12.
DriverVersion decode_driver_version(std::uint32_t vendor_id, std::uint32_t driver_id, std::uint32_t version,
                                    std::string_view os);
// A Vulkan API version: "1.3.280".
std::string api_version_text(std::uint32_t version);
std::string vendor_name(std::uint32_t vendor_id);

// What a workaround changes: bits, so that several can apply.
enum Workaround : std::uint32_t {
  kWorkaroundProcessor2D = 1u << 0,  // The processor draws the 2D pictures (as Debug, 2D by the processor).
  kWorkaroundProcessor3D = 1u << 1,  // The processor draws the 3D scenes (as Settings, Graphics, Processor).
  kWorkaroundVsync = 1u << 2,        // Vsync on: the present mode that waits for the display.
};
// The names of the bits set in `workarounds`, joined with ", ".
std::string workaround_names(std::uint32_t workarounds);

// A known GPU issue. Each test is passed when its field is 0 or empty.
struct GpuIssue {
  std::uint32_t vendor_id = 0;
  std::uint32_t device_min = 0, device_max = 0;  // Device IDs, inclusive; both 0 for any.
  std::string_view driver;                       // A part of the driver's name, any case.
  std::string_view os;
  std::uint64_t version_min = 0, version_max = 0;  // DriverVersion::comparable(), inclusive; both 0 for any.
  std::string_view problem;
  std::uint32_t workarounds = 0;
};

// This project's table: empty, or the issues proven here.
std::span<const GpuIssue> known_gpu_issues();
// The entries of `table` the GPU matches.
std::vector<const GpuIssue*> match_gpu_issues(const GpuDescription& gpu, std::span<const GpuIssue> table);

}  // namespace bd::debug
