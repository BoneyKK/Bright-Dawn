// What building the game library from the player's ROM needs, found on this
// computer: the builder's files and Python. The compiler is the builder's to
// fetch (tools/gamebuilder/build_game.py); Python, where there is none (as on
// most Windows computers), is the launcher's (app/fetch.h).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bd::app {

struct FoundTool {
  bool found = false;
  std::vector<std::string> command;  // How to run it: the program, perhaps with arguments first.
  std::string version;               // As it reports it.
  std::string problem;               // Why it cannot be used, when it cannot.
};

// The builder's files: tools/gamebuilder/build_game.py, the translator and
// the runtime's headers it compiles against. In a package, "builder" next
// to the program; when run from a build of the source tree, the source tree
// above it. Nothing if neither is there.
std::optional<std::filesystem::path> find_builder();
// Python 3.10 or later.
FoundTool find_python();

// Python to download for the build where there is none: CPython for 64-bit
// Windows, as the Python project publishes it on NuGet, pinned and checked
// by its SHA-256. Nothing on other systems, which have Python or a package
// of it.
struct Download {
  std::string what;    // For people: "Python 3.13.16".
  std::string url;
  std::string sha256;  // Lowercase hexadecimal.
  std::uint64_t size;  // Bytes, for showing.
  std::string member;  // The folder of the archive that is wanted.
  std::string program; // The program in that folder.
};
std::optional<Download> python_download();

// Where the game library is kept: game/bright-dawn-game.dll (or .so) in the
// user data directory.
std::filesystem::path game_library_path(const std::filesystem::path& user_data);

// A path as UTF-8 text, for showing and for passing to programs.
std::string utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(const std::string& text);

}  // namespace bd::app
