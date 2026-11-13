#include "nds/program.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace bd::nds {
namespace {

// The part of the ROM header the console's firmware copies into memory.
constexpr std::size_t kHeaderCopySize = 0x170;

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return Error{ErrorCode::Io, "cannot open " + path.string()};
  // Program files are a few megabytes at most; anything larger is a mistake.
  if (size > (std::uintmax_t{64} << 20)) return Error{ErrorCode::Corrupt, path.string() + " is too large to be program code"};
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot open " + path.string()};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    return Error{ErrorCode::Io, "cannot read " + path.string()};
  return bytes;
}

// A manifest line: a kind followed by key=value fields.
struct Line {
  std::string kind;
  std::map<std::string, std::string> fields;
};

bool parse_number(const std::string& text, std::uint32_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
  if (end == nullptr || *end != '\0' || value > 0xFFFFFFFFull) return false;
  *out = static_cast<std::uint32_t>(value);
  return true;
}

Status number_field(const Line& line, int line_number, const char* name, std::uint32_t* out) {
  const auto it = line.fields.find(name);
  if (it == line.fields.end() || !parse_number(it->second, out)) {
    return Error{ErrorCode::Corrupt, "manifest line " + std::to_string(line_number) + ": '" + line.kind +
                                         "' needs a numeric field '" + name + "'"};
  }
  return {};
}

Result<std::filesystem::path> file_field(const Line& line, int line_number,
                                         const std::filesystem::path& directory) {
  const auto it = line.fields.find("file");
  // A plain file name only: the manifest must not be able to name a file
  // outside its own directory.
  if (it == line.fields.end() || it->second.empty() ||
      it->second.find_first_of("/\\:") != std::string::npos || it->second == "." || it->second == "..") {
    return Error{ErrorCode::Corrupt, "manifest line " + std::to_string(line_number) + ": '" + line.kind +
                                         "' needs a field 'file' naming a file in the same directory"};
  }
  return directory / it->second;
}

}  // namespace

Result<Program> program_from_rom(const rom::Rom& rom) {
  Program program;
  auto arm9 = rom.arm9_decompressed();
  if (!arm9) return arm9.error();
  program.arm9 = std::move(arm9).value();
  program.arm9_load_address = rom.header().arm9.ram_address;
  program.arm9_entry = rom.header().arm9.entry_address;
  const ByteSpan bytes = rom.bytes();
  program.header.assign(bytes.begin(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(std::min(bytes.size(), kHeaderCopySize)));
  for (const rom::Overlay& overlay : rom.arm9_overlays()) {
    auto image = rom.load_overlay(overlay);
    if (!image) {
      return Error{image.error().code,
                   "overlay " + std::to_string(overlay.id) + ": " + image.error().message};
    }
    if (image.value().data.size() != overlay.ram_size) {
      return Error{ErrorCode::Corrupt, "overlay " + std::to_string(overlay.id) + " is " +
                                           std::to_string(image.value().data.size()) + " bytes once decoded, but " +
                                           std::to_string(overlay.ram_size) + " are expected"};
    }
    OverlayCode code;
    code.id = static_cast<int>(overlay.id);
    code.load_address = overlay.ram_address;
    code.bss_size = overlay.bss_size;
    code.static_init_start = overlay.static_init_start;
    code.static_init_end = overlay.static_init_end;
    code.image = std::move(image.value().data);
    program.overlays.push_back(std::move(code));
  }
  return program;
}

Result<Program> program_from_directory(const std::filesystem::path& directory) {
  const std::filesystem::path manifest_path = directory / "manifest.txt";
  std::ifstream in(manifest_path);
  if (!in) return Error{ErrorCode::Io, "cannot open " + manifest_path.string()};

  Program program;
  bool have_arm9 = false;
  std::string text;
  for (int line_number = 1; std::getline(in, text); ++line_number) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    std::istringstream words(text);
    Line line;
    if (!(words >> line.kind) || line.kind[0] == '#') continue;
    for (std::string word; words >> word;) {
      const std::size_t equals = word.find('=');
      if (equals == std::string::npos || equals == 0) {
        return Error{ErrorCode::Corrupt, "manifest line " + std::to_string(line_number) +
                                             ": expected key=value, found '" + word + "'"};
      }
      line.fields[word.substr(0, equals)] = word.substr(equals + 1);
    }

    if (line.kind == "arm9") {
      if (have_arm9)
        return Error{ErrorCode::Corrupt, "manifest line " + std::to_string(line_number) + ": a second 'arm9'"};
      if (auto s = number_field(line, line_number, "load", &program.arm9_load_address); !s) return s.error();
      if (auto s = number_field(line, line_number, "entry", &program.arm9_entry); !s) return s.error();
      auto path = file_field(line, line_number, directory);
      if (!path) return path.error();
      auto bytes = read_file(path.value());
      if (!bytes) return bytes.error();
      program.arm9 = std::move(bytes).value();
      have_arm9 = true;
    } else if (line.kind == "overlay9") {
      OverlayCode code;
      std::uint32_t id = 0, size = 0;
      if (auto s = number_field(line, line_number, "id", &id); !s) return s.error();
      if (auto s = number_field(line, line_number, "load", &code.load_address); !s) return s.error();
      if (auto s = number_field(line, line_number, "size", &size); !s) return s.error();
      if (auto s = number_field(line, line_number, "bss", &code.bss_size); !s) return s.error();
      if (auto s = number_field(line, line_number, "init_start", &code.static_init_start); !s) return s.error();
      if (auto s = number_field(line, line_number, "init_end", &code.static_init_end); !s) return s.error();
      auto path = file_field(line, line_number, directory);
      if (!path) return path.error();
      auto bytes = read_file(path.value());
      if (!bytes) return bytes.error();
      if (bytes.value().size() != size) {
        return Error{ErrorCode::Corrupt, path.value().string() + " is " + std::to_string(bytes.value().size()) +
                                             " bytes, but the manifest says " + std::to_string(size)};
      }
      code.id = static_cast<int>(id);
      code.image = std::move(bytes).value();
      program.overlays.push_back(std::move(code));
    }
    // Other kinds (the ARM7 program, anything added later) are not needed here.
  }
  if (in.bad()) return Error{ErrorCode::Io, "cannot read " + manifest_path.string()};
  if (!have_arm9) return Error{ErrorCode::Corrupt, manifest_path.string() + " has no 'arm9' line"};
  return program;
}

}  // namespace bd::nds
