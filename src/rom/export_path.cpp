#include "rom/export_path.h"

#include <cstdio>
#include <string>

namespace bd::rom {
namespace {

bool is_reserved(unsigned char c) {
  switch (c) {
    case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|': case '%':
      return true;
    default:
      return c < 0x20 || c >= 0x7F;
  }
}

}  // namespace

std::optional<std::filesystem::path> to_export_path(std::string_view rom_path) {
  std::filesystem::path out;
  std::size_t start = 0;
  while (start <= rom_path.size()) {
    std::size_t end = rom_path.find('/', start);
    if (end == std::string_view::npos) end = rom_path.size();
    const std::string_view part = rom_path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return std::nullopt;

    // Reserved and non-ASCII bytes are written as %XX so that distinct ROM
    // names stay distinct on disk.
    std::string safe;
    for (char ch : part) {
      const auto c = static_cast<unsigned char>(ch);
      if (is_reserved(c)) {
        char hex[4];
        std::snprintf(hex, sizeof hex, "%%%02X", static_cast<unsigned>(c));
        safe += hex;
      } else {
        safe += ch;
      }
    }
    // Windows silently drops trailing dots and spaces.
    if (safe.back() == '.' || safe.back() == ' ') safe += "%";
    out /= safe;
    start = end + 1;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

}  // namespace bd::rom
