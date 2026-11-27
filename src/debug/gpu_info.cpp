#include "debug/gpu_info.h"

#include <algorithm>
#include <cstdio>

namespace bd::debug {

std::string DriverVersion::text() const {
  std::string t;
  for (int i = 0; i < count; ++i) {
    if (i != 0) t += '.';
    // NVIDIA writes its minor number with two digits ("560.94"; "535.04").
    char part[16];
    std::snprintf(part, sizeof part, scheme == "nvidia" && i == 1 ? "%02u" : "%u", parts[i]);
    t += part;
  }
  return t;
}

std::uint64_t DriverVersion::comparable() const {
  std::uint64_t n = 0;
  for (int i = 0; i < 4; ++i) n = n << 16 | (i < count ? std::min<std::uint32_t>(parts[i], 0xFFFF) : 0);
  return n;
}

DriverVersion decode_driver_version(std::uint32_t vendor_id, std::uint32_t driver_id, std::uint32_t v,
                                    std::string_view os) {
  DriverVersion d;
  // NVIDIA's own driver (not Mesa's NVK, which numbers as Mesa does).
  const bool nvidia = driver_id == kDriverNvidiaProprietary || (driver_id == 0 && vendor_id == kVendorNvidia);
  // Intel's own driver on Windows (not Mesa's ANV on Linux).
  const bool intel_windows = driver_id == kDriverIntelWindows || (driver_id == 0 && vendor_id == kVendorIntel && os == "windows");
  if (nvidia) {
    d.scheme = "nvidia";
    d.parts[0] = v >> 22;
    d.parts[1] = (v >> 14) & 0xFF;
    d.parts[2] = (v >> 6) & 0xFF;
    d.parts[3] = v & 0x3F;
    d.count = 4;
  } else if (intel_windows) {
    d.scheme = "intel-windows";
    d.parts[0] = v >> 14;
    d.parts[1] = v & 0x3FFF;
    d.count = 2;
  } else {
    d.scheme = "vulkan";
    d.parts[0] = v >> 22;
    d.parts[1] = (v >> 12) & 0x3FF;
    d.parts[2] = v & 0xFFF;
    d.count = 3;
  }
  return d;
}

std::string api_version_text(std::uint32_t v) {
  char text[32];
  std::snprintf(text, sizeof text, "%u.%u.%u", (v >> 22) & 0x7F, (v >> 12) & 0x3FF, v & 0xFFF);
  return text;
}

std::string vendor_name(std::uint32_t vendor_id) {
  switch (vendor_id) {
    case kVendorAmd: return "AMD";
    case kVendorNvidia: return "NVIDIA";
    case kVendorIntel: return "Intel";
    case kVendorApple: return "Apple";
    case kVendorQualcomm: return "Qualcomm";
    case kVendorArm: return "Arm";
    case kVendorMesa: return "Mesa (software)";
    case 0: return "unknown";
    default: {
      char text[16];
      std::snprintf(text, sizeof text, "0x%04X", vendor_id);
      return text;
    }
  }
}

std::string workaround_names(std::uint32_t w) {
  std::string names;
  const auto add = [&](std::uint32_t bit, const char* name) {
    if ((w & bit) == 0) return;
    if (!names.empty()) names += ", ";
    names += name;
  };
  add(kWorkaroundProcessor2D, "2D by the processor");
  add(kWorkaroundProcessor3D, "3D by the processor");
  add(kWorkaroundVsync, "vsync on");
  return names;
}

namespace {

// Known GPU issues: none proven in this project yet. Add an entry only when
// a performance report (or a log) shows the issue; say which in a comment.
constexpr GpuIssue kNone{};
constexpr std::span<const GpuIssue> kKnownIssues(&kNone, 0);

bool contains_any_case(std::string_view text, std::string_view part) {
  if (part.empty()) return true;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  return std::search(text.begin(), text.end(), part.begin(), part.end(),
                     [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

}  // namespace

std::span<const GpuIssue> known_gpu_issues() { return kKnownIssues; }

std::vector<const GpuIssue*> match_gpu_issues(const GpuDescription& gpu, std::span<const GpuIssue> table) {
  std::vector<const GpuIssue*> found;
  const std::uint64_t version = decode_driver_version(gpu.vendor_id, gpu.driver_id, gpu.driver_version, gpu.os).comparable();
  for (const GpuIssue& issue : table) {
    if (issue.vendor_id != 0 && issue.vendor_id != gpu.vendor_id) continue;
    if ((issue.device_min != 0 || issue.device_max != 0) &&
        (gpu.device_id < issue.device_min || gpu.device_id > issue.device_max))
      continue;
    if (!issue.driver.empty() && !contains_any_case(gpu.driver_name, issue.driver) &&
        !contains_any_case(gpu.sdl_driver_name, issue.driver))
      continue;
    if (!issue.os.empty() && issue.os != gpu.os) continue;
    if ((issue.version_min != 0 || issue.version_max != 0) &&
        (gpu.driver_version == 0 || version < issue.version_min || (issue.version_max != 0 && version > issue.version_max)))
      continue;
    found.push_back(&issue);
  }
  return found;
}

}  // namespace bd::debug
