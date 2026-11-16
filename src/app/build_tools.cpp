#include "app/build_tools.h"

#include <SDL3/SDL.h>

#include <system_error>

#include "app/child_process.h"
#include "game_library_host.h"
#include "ui/build_progress.h"

namespace bd::app {
namespace {

bool is_builder(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::is_regular_file(folder / "tools" / "gamebuilder" / "build_game.py", ec) &&
         std::filesystem::is_regular_file(folder / "tools" / "translator" / "translate_game.py", ec) &&
         std::filesystem::is_regular_file(folder / "src" / "runtime" / "game_library_glue.cpp", ec);
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

std::optional<Download> python_download() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
  return Download{"Python 3.13.16",
                  "https://api.nuget.org/v3-flatcontainer/python/3.13.16/python.3.13.16.nupkg",
                  "95fad176338f1d6a799e45946488063f4aa61764e6cff2f132ddeffbf7a04c34",
                  14817881,
                  "tools",
                  "tools/python.exe"};
#else
  return std::nullopt;
#endif
}

std::filesystem::path game_library_path(const std::filesystem::path& user_data) {
  return user_data / "game" / runtime::GameLibrary::file_name();
}

}  // namespace bd::app
