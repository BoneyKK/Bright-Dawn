#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS  // std::getenv is used read-only here.
#endif

#include "platform/paths.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace bd::platform {
namespace {

std::filesystem::path env_path(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return {};
  return std::filesystem::path(value);
}

}  // namespace

Result<std::filesystem::path> user_data_dir() {
#if defined(_WIN32)
  if (auto base = env_path("LOCALAPPDATA"); !base.empty()) return base / "BrightDawn";
  return Error{ErrorCode::Io, "LOCALAPPDATA is not set; cannot locate the user data directory"};
#elif defined(__APPLE__)
  if (auto home = env_path("HOME"); !home.empty())
    return home / "Library" / "Application Support" / "BrightDawn";
  return Error{ErrorCode::Io, "HOME is not set; cannot locate the user data directory"};
#else
  if (auto xdg = env_path("XDG_DATA_HOME"); !xdg.empty() && xdg.is_absolute())
    return xdg / "bright-dawn";
  if (auto home = env_path("HOME"); !home.empty()) return home / ".local" / "share" / "bright-dawn";
  return Error{ErrorCode::Io,
               "neither XDG_DATA_HOME nor HOME is set; cannot locate the user data directory"};
#endif
}

Result<std::filesystem::path> executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return Error{ErrorCode::Io, "the program's own path is not known"};
    if (n < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), n));
    if (buffer.size() >= 32768) return Error{ErrorCode::Io, "the program's own path is too long"};
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return Error{ErrorCode::Io, "the program's own path is not known"};
  std::error_code ec;
  const std::filesystem::path real = std::filesystem::canonical(buffer.data(), ec);
  return ec ? std::filesystem::path(buffer.data()) : real;
#else
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return Error{ErrorCode::Io, "the program's own path is not known: " + ec.message()};
  return path;
#endif
}

}  // namespace bd::platform
