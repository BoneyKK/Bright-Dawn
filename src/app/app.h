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
#include "ui/problem_report.h"
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
  std::uint64_t capture_every = 0;
  // Write the 3D scene of the last frame here (nds/scene_dump.h).
  std::filesystem::path dump_3d;
  // Draw the 2D pictures with the processor too, and log where the graphics
  // hardware's differ.
  bool check_2d = false;
  std::filesystem::path record_sound;  // Keep the game's sound in this WAV file.  // Also write a frame every so many, named FILE-frame.bmp.
  std::vector<input::ScriptedPress> script;
  std::vector<input::ScriptedTouch> touches;
  std::vector<input::ScriptedStick> sticks;
};

struct App {
  Options options;
  platform::Settings settings;
  std::filesystem::path user_data;     // Empty if there is none.
  std::filesystem::path settings_path;  // Empty: not saved.
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  // The GPU device the renderer draws with, when it is SDL's GPU renderer
  // (on Vulkan): the graphics hardware's 3D scenes use it too. Destroyed
  // after the renderer.
  SDL_GPUDevice* gpu = nullptr;
  Gui gui;
  // The translated code linked in (bright-dawn-game), or an empty table.
  const TranslatedEntry* translation = nullptr;
  bool translated_build = false;
  // Whether the game being played runs translated code (linked in, or the
  // game built from the player's ROM), not only the interpreter.
  bool native_code = false;
  // What "Report a problem" fills in; kept up to date as the run goes on.
  ui::ProblemReport report;

  explicit App(platform::Settings s) : settings(std::move(s)) {}

  // The player changed these settings: saved, and logged.
  void settings_changed(const std::vector<std::string>& keys);
  void set_setting(std::string_view key, std::string_view value);
};

// Shows a message in a box, as well as logging it, since on Windows the
// window program has no console to print to.
void complain(SDL_Window* window, const std::string& message);

// Opens a folder in the system's file manager, making it first if need be.
void open_folder(const std::filesystem::path& folder);

// Where the frame log is written (launcher, Debug page): next to the
// program.
std::filesystem::path frame_log_path();

// "Report a problem": opens the project's bug form in the browser, with what
// is known of this run filled in, and the folder of the logs, so that the
// log can be attached.
void report_problem(const App& app);

}  // namespace bd::app
