// What the window program's parts share: the command line, the settings,
// the window and its renderer, and the program's own screens.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "app/gui.h"
#include "input/bindings.h"
#include "platform/settings.h"
#include "translated_table.h"

namespace bd::app {

// What the command line asked for besides settings, which go into the
// settings as values for this run.
struct Options {
  std::filesystem::path source;  // A ROM, or a folder extracted from one; empty: the launcher.
  bool unthrottled = false;
  bool interpret_only = false;
  std::uint64_t frames = 0;
  std::string capture;
  std::vector<input::ScriptedPress> script;
  std::vector<input::ScriptedTouch> touches;
};

struct App {
  Options options;
  platform::Settings settings;
  std::filesystem::path user_data;     // Empty if there is none.
  std::filesystem::path settings_path;  // Empty: not saved.
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  Gui gui;
  // The translated code linked in (bright-dawn-game), or an empty table.
  const TranslatedEntry* translation = nullptr;
  bool translated_build = false;

  explicit App(platform::Settings s) : settings(std::move(s)) {}

  // The player changed these settings: saved, and logged.
  void settings_changed(const std::vector<std::string>& keys);
  void set_setting(std::string_view key, std::string_view value);
};

// Shows a message in a box, as well as logging it, since on Windows the
// window program has no console to print to.
void complain(SDL_Window* window, const std::string& message);

}  // namespace bd::app
