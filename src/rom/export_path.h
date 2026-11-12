#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace bd::rom {

// Converts a path taken from a ROM into a relative path that is safe to
// create on the host: no absolute paths, no ".." components, and no characters
// that are reserved on common file systems. Returns nullopt when the path
// cannot be made safe (for example, it contains a ".." component).
std::optional<std::filesystem::path> to_export_path(std::string_view rom_path);

}  // namespace bd::rom
