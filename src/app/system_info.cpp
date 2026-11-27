#include "app/system_info.h"

#include <SDL3/SDL_vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include "core/version.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#endif

#ifndef BD_BUILD_TYPE
#define BD_BUILD_TYPE ""
#endif

namespace bd::app {

namespace {

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

#if defined(__APPLE__)
std::string sysctl_text(const char* name) {
  char value[256] = {};
  std::size_t size = sizeof value - 1;
  if (sysctlbyname(name, value, &size, nullptr, 0) != 0) return {};
  return trimmed(value);
}
int sysctl_int(const char* name) {
  int value = 0;
  std::size_t size = sizeof value;
  return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : 0;
}
#endif

// The OS and its version.
std::string os_version() {
#if defined(_WIN32)
  // RtlGetVersion gives the real version (GetVersionEx is told a lie for
  // programs without a manifest that names the newer systems).
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (get != nullptr && get(&info) == 0) {
      char text[96];
      // Windows 11 is version 10.0 with build 22000 or later.
      std::snprintf(text, sizeof text, "Windows %s (%lu.%lu, build %lu)",
                    info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000 ? "11" : std::to_string(info.dwMajorVersion).c_str(),
                    info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
      return text;
    }
  }
  return "Windows";
#elif defined(__APPLE__)
  const std::string v = sysctl_text("kern.osproductversion");
  return "macOS " + (v.empty() ? std::string("(version unknown)") : v);
#else
  std::string name;
  std::ifstream release("/etc/os-release");
  for (std::string line; std::getline(release, line);) {
    if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
    name = line.substr(12);
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
  }
  utsname u{};
  const std::string kernel = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release : "";
  if (name.empty()) return kernel.empty() ? SDL_GetPlatform() : kernel;
  return kernel.empty() ? name : name + " (" + kernel + ")";
#endif
}

std::string cpu_model() {
#if defined(_WIN32)
  char value[256] = {};
  DWORD size = sizeof value;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
    return trimmed(value);
  return {};
#elif defined(__APPLE__)
  return sysctl_text("machdep.cpu.brand_string");
#else
  std::ifstream info("/proc/cpuinfo");
  std::string fallback;
  for (std::string line; std::getline(info, line);) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trimmed(line.substr(0, colon));
    if (key == "model name") return trimmed(line.substr(colon + 1));
    if ((key == "Hardware" || key == "Model") && fallback.empty()) fallback = trimmed(line.substr(colon + 1));
  }
  return fallback;
#endif
}

// The processor's cores (not threads); 0 when it cannot be told.
int cpu_cores() {
#if defined(_WIN32)
  DWORD size = 0;
  GetLogicalProcessorInformation(nullptr, &size);
  std::string buffer(size, '\0');
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buffer.data());
  if (size == 0 || !GetLogicalProcessorInformation(info, &size)) return 0;
  int cores = 0;
  for (std::size_t i = 0; i < size / sizeof *info; ++i)
    if (info[i].Relationship == RelationProcessorCore) ++cores;
  return cores;
#elif defined(__APPLE__)
  return sysctl_int("hw.physicalcpu");
#else
  std::ifstream info("/proc/cpuinfo");
  std::set<std::pair<std::string, std::string>> cores;
  std::string package;
  for (std::string line; std::getline(info, line);) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trimmed(line.substr(0, colon));
    if (key == "physical id") package = trimmed(line.substr(colon + 1));
    if (key == "core id") cores.insert({package, trimmed(line.substr(colon + 1))});
  }
  return static_cast<int>(cores.size());
#endif
}

// --- A Vulkan instance of our own, to ask what SDL's GPU device does not
// say. Only the parts of Vulkan's structures that are read, as the
// specification lays them out (64-bit).

using VkFlags = std::uint32_t;
using VkInstance = struct VkInstanceOpaque*;
using VkPhysicalDevice = struct VkPhysicalDeviceOpaque*;
using VkResult = std::int32_t;
using PFN_vkVoidFunction = void (*)();
#if defined(_WIN32)
#define BD_VKAPI __stdcall
#else
#define BD_VKAPI
#endif
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(BD_VKAPI*)(VkInstance, const char*);

struct VkApplicationInfo {
  std::int32_t sType = 0;  // VK_STRUCTURE_TYPE_APPLICATION_INFO
  const void* pNext = nullptr;
  const char* pApplicationName = nullptr;
  std::uint32_t applicationVersion = 0;
  const char* pEngineName = nullptr;
  std::uint32_t engineVersion = 0;
  std::uint32_t apiVersion = 0;
};
struct VkInstanceCreateInfo {
  std::int32_t sType = 1;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
  const void* pNext = nullptr;
  VkFlags flags = 0;
  const VkApplicationInfo* pApplicationInfo = nullptr;
  std::uint32_t enabledLayerCount = 0;
  const char* const* ppEnabledLayerNames = nullptr;
  std::uint32_t enabledExtensionCount = 0;
  const char* const* ppEnabledExtensionNames = nullptr;
};
struct VkPhysicalDeviceProperties {
  std::uint32_t apiVersion, driverVersion, vendorID, deviceID;
  std::int32_t deviceType;
  char deviceName[256];
  std::uint8_t pipelineCacheUUID[16];
  alignas(8) std::uint8_t limits_and_sparse[1024];  // VkPhysicalDeviceLimits (504 bytes) and sparse properties: not read.
};
struct VkPhysicalDeviceDriverProperties {
  std::int32_t sType = 1000196000;  // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
  void* pNext = nullptr;
  std::uint32_t driverID = 0;
  char driverName[256] = {};
  char driverInfo[256] = {};
  std::uint8_t conformanceVersion[4] = {};
};
struct VkPhysicalDeviceProperties2 {
  std::int32_t sType = 1000059001;  // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
  void* pNext = nullptr;
  VkPhysicalDeviceProperties properties{};
};
struct VkMemoryHeap {
  std::uint64_t size;
  VkFlags flags;
};
struct VkPhysicalDeviceMemoryProperties {
  std::uint32_t memoryTypeCount;
  std::uint32_t memoryTypes[32][2];
  std::uint32_t memoryHeapCount;
  VkMemoryHeap memoryHeaps[16];
};
static_assert(offsetof(VkPhysicalDeviceProperties, deviceName) == 20);
static_assert(offsetof(VkPhysicalDeviceDriverProperties, driverID) == 16);
static_assert(offsetof(VkPhysicalDeviceMemoryProperties, memoryHeaps) == 264);

using PFN_vkCreateInstance = VkResult(BD_VKAPI*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(BD_VKAPI*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(BD_VKAPI*)(VkInstance, std::uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties2 = void(BD_VKAPI*)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
using PFN_vkGetPhysicalDeviceMemoryProperties = void(BD_VKAPI*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);

constexpr std::uint32_t kVulkan11 = 1u << 22 | 1u << 12;

const char* device_type_name(std::int32_t type) {
  switch (type) {
    case 1: return "integrated";
    case 2: return "discrete";
    case 3: return "virtual";
    case 4: return "cpu";
    default: return "other";
  }
}

// Fills in what Vulkan says of the device named `gpu.name` (or of the first,
// when SDL gave no name). False when Vulkan cannot be asked.
bool ask_vulkan(debug::GpuDescription& gpu) {
  // SDL counts the loads: the GPU device's stays.
  if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
  struct Unload {
    ~Unload() { SDL_Vulkan_UnloadLibrary(); }
  } unload;
  auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (get == nullptr) return false;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
  if (create == nullptr) return false;
  VkApplicationInfo app;
  app.pApplicationName = "Bright Dawn performance report";
  app.apiVersion = kVulkan11;
  VkInstanceCreateInfo info;
  info.pApplicationInfo = &app;
#if defined(__APPLE__)
  // MoltenVK is listed only to those who ask for portability drivers.
  const char* extensions[] = {"VK_KHR_portability_enumeration"};
  info.flags = 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
  info.enabledExtensionCount = 1;
  info.ppEnabledExtensionNames = extensions;
#endif
  VkInstance instance = nullptr;
  if (create(&info, nullptr, &instance) != 0 || instance == nullptr) return false;
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
  auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(instance, "vkGetPhysicalDeviceProperties2"));
  auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"));
  bool found = false;
  std::uint32_t count = 0;
  if (enumerate != nullptr && properties2 != nullptr && enumerate(instance, &count, nullptr) == 0 && count != 0) {
    std::string devices_buffer(sizeof(VkPhysicalDevice) * count, '\0');
    auto* devices = reinterpret_cast<VkPhysicalDevice*>(devices_buffer.data());
    if (enumerate(instance, &count, devices) >= 0) {
      for (std::uint32_t i = 0; i < count && !found; ++i) {
        VkPhysicalDeviceDriverProperties driver;
        VkPhysicalDeviceProperties2 props;
        props.pNext = &driver;
        properties2(devices[i], &props);
        const VkPhysicalDeviceProperties& p = props.properties;
        if (!gpu.name.empty() && gpu.name != p.deviceName && i + 1 < count) continue;
        found = true;
        if (gpu.name.empty()) gpu.name = p.deviceName;
        gpu.vendor_id = p.vendorID;
        gpu.device_id = p.deviceID;
        gpu.device_type = device_type_name(p.deviceType);
        gpu.driver_version = p.driverVersion;
        gpu.api_version = p.apiVersion;
        gpu.driver_id = driver.driverID;
        gpu.driver_name = driver.driverName;
        gpu.driver_info = driver.driverInfo;
        if (memory != nullptr) {
          VkPhysicalDeviceMemoryProperties m{};
          memory(devices[i], &m);
          for (std::uint32_t h = 0; h < m.memoryHeapCount && h < 16; ++h)
            if ((m.memoryHeaps[h].flags & 1u) != 0) gpu.vram_bytes += m.memoryHeaps[h].size;  // DEVICE_LOCAL
        }
      }
    }
  }
  if (destroy != nullptr) destroy(instance, nullptr);
  return found;
}

}  // namespace

std::string os_id() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return SDL_GetPlatform();
#endif
}

debug::PerfReport::Section system_section() {
  debug::PerfReport::Section s;
  s.name = "system";
  const std::string os = os_version();
  const std::string cpu = cpu_model();
  const int cores = cpu_cores(), threads = SDL_GetNumLogicalCPUCores();
  const int ram_mb = SDL_GetSystemRAM();
  std::string build_type = BD_BUILD_TYPE;
  if (build_type.empty()) {
#if defined(NDEBUG)
    build_type = "release (no type named)";
#else
    build_type = "debug (no type named)";
#endif
  }
  char summary[512];
  std::snprintf(summary, sizeof summary, "%s; %s, %d cores, %d threads; %.1f GB of memory; %s build %s", os.c_str(),
                cpu.empty() ? "processor unknown" : cpu.c_str(), cores, threads, ram_mb / 1024.0, build_type.c_str(),
                BD_VERSION);
  s.text("summary", summary);
  s.text("os", os);
  s.text("platform", SDL_GetPlatform());
  s.text("cpu", cpu.empty() ? "unknown" : cpu);
  s.number("cpu_cores", cores);
  s.number("cpu_threads", threads);
  s.number("ram_mb", ram_mb);
  s.text("version", BD_VERSION);
  s.text("build_type", build_type);
  s.text("commit", BD_COMMIT[0] != '\0' ? BD_COMMIT : "unknown");
  s.text("sdl", std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION) + "." +
                    std::to_string(SDL_MICRO_VERSION));
  return s;
}

debug::GpuDescription describe_gpu(SDL_GPUDevice* device, bool ask_driver) {
  debug::GpuDescription gpu;
  gpu.os = os_id();
  if (device == nullptr) return gpu;
  if (const char* d = SDL_GetGPUDeviceDriver(device)) gpu.backend = d;
  const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
  const auto text = [&](const char* name) {
    const char* v = SDL_GetStringProperty(props, name, nullptr);
    return std::string(v != nullptr ? v : "");
  };
  gpu.name = text(SDL_PROP_GPU_DEVICE_NAME_STRING);
  gpu.sdl_driver_name = text(SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING);
  gpu.sdl_driver_version = text(SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING);
  gpu.sdl_driver_info = text(SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING);
  if (ask_driver && gpu.backend == "vulkan") ask_vulkan(gpu);
  return gpu;
}

}  // namespace bd::app
