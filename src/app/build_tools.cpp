#include "app/build_tools.h"

#include <SDL3/SDL.h>

#include <system_error>

#include "app/child_process.h"
#include "core/version.h"
#include "ui/build_progress.h"

namespace bd::app {
namespace {

bool is_builder(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::is_regular_file(folder / "tools" / "gamebuilder" / "build_game.py", ec) &&
         std::filesystem::is_regular_file(folder / "CMakeLists.txt", ec) &&
         std::filesystem::is_directory(folder / "src", ec);
}

FoundTool find_tool(const std::vector<std::vector<std::string>>& candidates, std::array<int, 3> least,
                    const char* name) {
  FoundTool best;
  best.problem = std::string(name) + " was not found";
  for (const std::vector<std::string>& command : candidates) {
    std::vector<std::string> ask = command;
    ask.emplace_back("--version");
    const ProgramOutput out = run_program(ask, 10000);
    if (!out.started || (ChildProcess::kExitCodeKnown && out.exit_code != 0)) continue;
    const auto version = ui::version_in(out.output);
    if (!version) continue;
    std::string text = std::to_string((*version)[0]) + "." + std::to_string((*version)[1]) + "." +
                       std::to_string((*version)[2]);
    if (*version < least) {
      best.problem = std::string(name) + " " + text + " was found; " + std::to_string(least[0]) + "." +
                     std::to_string(least[1]) + " or later is needed";
      continue;
    }
    best.found = true;
    best.command = command;
    best.version = text;
    best.problem.clear();
    return best;
  }
  return best;
}

}  // namespace

std::string utf8(const std::filesystem::path& path) {
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::optional<std::filesystem::path> find_builder() {
  const char* base_text = SDL_GetBasePath();
  if (base_text == nullptr) return std::nullopt;
  const std::filesystem::path base = from_utf8(base_text);
  if (is_builder(base / "builder")) return base / "builder";
  // A build of the source tree: the program is in the build folder, which
  // is in the source tree or next to it.
  std::filesystem::path folder = base;
  for (int up = 0; up < 4 && folder.has_parent_path(); ++up) {
    if (is_builder(folder)) return folder;
    const std::filesystem::path parent = folder.parent_path();
    if (parent == folder) break;
    folder = parent;
  }
  return std::nullopt;
}

FoundTool find_python() {
#if defined(_WIN32)
  // The Python launcher first: "python" may be the Store's placeholder.
  return find_tool({{"py", "-3"}, {"python"}, {"python3"}}, {3, 10, 0}, "Python");
#else
  return find_tool({{"python3"}, {"python"}}, {3, 10, 0}, "Python");
#endif
}

FoundTool find_cmake() { return find_tool({{"cmake"}}, {3, 20, 0}, "CMake"); }

std::optional<std::filesystem::path> built_game(const std::filesystem::path& work) {
#if defined(_WIN32)
  const char* name = "bright-dawn-game.exe";
#else
  const char* name = "bright-dawn-game";
#endif
  // As the builder looks for it: in the build folder, or in a folder of the
  // configuration, for Visual Studio.
  std::error_code ec;
  for (const std::filesystem::path& p : {work / "game" / name, work / "game" / "Release" / name})
    if (std::filesystem::is_regular_file(p, ec)) return p;
  return std::nullopt;
}

BuiltGameVersion built_game_version(const std::filesystem::path& game) {
  BuiltGameVersion result;
  const ProgramOutput out = run_program({utf8(game), "--version"}, 10000);
  if (!out.started) {
    result.problem = "it could not be started";
    return result;
  }
  std::string line = out.output.substr(0, out.output.find('\n'));
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if ((ChildProcess::kExitCodeKnown && out.exit_code != 0) || line.rfind("bright-dawn ", 0) != 0) {
    result.problem = "it did not say its version";
    return result;
  }
  result.known = true;
  result.version = line;
  return result;
}

std::string own_version_line() { return std::string("bright-dawn ") + BD_VERSION; }

}  // namespace bd::app
