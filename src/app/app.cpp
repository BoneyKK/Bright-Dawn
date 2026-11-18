#include "app/app.h"

#include <cstdio>

#include "core/log.h"
#include "app/build_tools.h"

namespace bd::app {

void App::settings_changed(const std::vector<std::string>& keys) {
  if (keys.empty()) return;
  for (const std::string& key : keys) log::info("settings", key + " = " + settings.value(key));
  if (settings_path.empty()) return;
  if (Status saved = settings.save(settings_path); !saved) log::warning("settings", saved.error().message);
}

void App::set_setting(std::string_view key, std::string_view value) {
  if (Status s = settings.set(key, value); !s) {
    log::warning("settings", s.error().message);
    return;
  }
  settings_changed({std::string(key)});
}

void complain(SDL_Window* window, const std::string& message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  log::error("message", message);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bright Dawn", message.c_str(), window);
}

void open_folder(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::string url = "file:///" + utf8(folder);
  for (char& c : url)
    if (c == '\\') c = '/';
  // "file:////home" is read as a network share on some systems.
  if (url.rfind("file:////", 0) == 0) url.erase(7, 1);
  if (!SDL_OpenURL(url.c_str())) log::warning("app", std::string("cannot open the folder: ") + SDL_GetError());
}

void report_problem(const App& app) {
  const std::string url = ui::problem_report_url(app.report);
  log::info("app", "report a problem: " + url);
  if (!SDL_OpenURL(url.c_str())) {
    log::warning("app", std::string("cannot open the browser: ") + SDL_GetError());
    complain(app.window, "The browser could not be opened. Please report the problem at\n" +
                             std::string(ui::kNewIssueUrl) + "\nand attach the log.");
  }
  if (const std::filesystem::path file = log::file_path(); !file.empty()) open_folder(file.parent_path());
}

}  // namespace bd::app
