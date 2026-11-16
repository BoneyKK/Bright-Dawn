// What building the game from the player's ROM needs, found on this
// computer: the builder's files, Python and CMake. And the game once built.
#pragma once

#include <array>
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

// The builder's files: the folder with tools/gamebuilder/build_game.py and
// the sources it builds. In a package, "builder" next to the program; when
// run from a build of the source tree, the source tree above it. Nothing if
// neither is there.
std::optional<std::filesystem::path> find_builder();
// Python 3.10 or later.
FoundTool find_python();
// CMake 3.20 or later.
FoundTool find_cmake();

// The game the builder makes, in its work folder, if it is there.
std::optional<std::filesystem::path> built_game(const std::filesystem::path& work);
// The version the built game names ("bright-dawn 0.1.0-pre.12 (abc1234)"),
// or why it could not be asked.
struct BuiltGameVersion {
  bool known = false;
  std::string version;
  std::string problem;
};
BuiltGameVersion built_game_version(const std::filesystem::path& game);
// This program's version, as the built game would name it.
std::string own_version_line();

// A path as UTF-8 text, for showing and for passing to programs.
std::string utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(const std::string& text);

}  // namespace bd::app
