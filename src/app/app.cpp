#include "app/app.h"

#include <cstdio>

#include "core/log.h"

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

}  // namespace bd::app
