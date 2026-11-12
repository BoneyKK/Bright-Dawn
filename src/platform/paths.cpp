#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS  // std::getenv is used read-only here.
#endif

#include "platform/paths.h"

#include <cstdlib>

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

}  // namespace bd::platform
