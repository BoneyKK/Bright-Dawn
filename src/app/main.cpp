// Bright Dawn: the game in a window.
//
// Started without a ROM, it shows the launcher (app/launcher.h): the ROM is
// chosen, the game built and the settings changed there, without the
// command line. Started with one (or with a folder extracted from it), it
// plays it at once. Either way the settings are those kept in the user data
// directory, which the command line can change for one run, and every run
// writes a log there (core/log.h).
//
// Built as bright-dawn, every instruction of the game is interpreted. Built
// as bright-dawn-game (BD_TRANSLATED_GAME), the game's code translated to C++
// from the user's own ROM is linked in and runs natively wherever it has
// been translated, with the interpreter behind it.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "app/build_tools.h"
#include "app/game_screen.h"
#include "app/launcher.h"
#include "core/log.h"
#include "core/version.h"
#include "gfx/screen_layout.h"
#include "input/bindings.h"
#include "nds/console.h"
#include "nds/keypad.h"
#include "nds/program.h"
#include "platform/crash_log.h"
#include "platform/paths.h"
#include "rom/game_id.h"
#include "rom/rom.h"
#include "translated_table.h"
#include "ui/game_settings.h"

namespace {

using namespace bd;
using app::App;
using app::complain;

const TranslatedEntry kNoTranslations[] = {{0, nullptr, -1, nullptr, 0}};

#ifdef BD_TRANSLATED_GAME
constexpr bool kHasTranslation = true;
const TranslatedEntry* const kTranslation = translated_table;
#else
constexpr bool kHasTranslation = false;
const TranslatedEntry* const kTranslation = kNoTranslations;
#endif

// The input module's button order is the keypad's.
constexpr bool same_button_order() {
  using bd::input::DsButton;
  using bd::nds::Button;
  constexpr std::array<std::pair<DsButton, Button>, 12> pairs = {{
      {DsButton::A, Button::A}, {DsButton::B, Button::B}, {DsButton::Select, Button::Select},
      {DsButton::Start, Button::Start}, {DsButton::Right, Button::Right}, {DsButton::Left, Button::Left},
      {DsButton::Up, Button::Up}, {DsButton::Down, Button::Down}, {DsButton::R, Button::R},
      {DsButton::L, Button::L}, {DsButton::X, Button::X}, {DsButton::Y, Button::Y},
  }};
  for (const auto& [a, b] : pairs)
    if (static_cast<unsigned>(a) != static_cast<unsigned>(b)) return false;
  return true;
}
static_assert(same_button_order());

constexpr const char* kUsage =
    "Usage: bright-dawn [options] [ROM file, or a directory extracted from it]\n"
    "\n"
    "Without a ROM, the launcher is shown: choose the ROM, build the game, change the\n"
    "settings. With one, it is played at once.\n"
    "\n"
    "These change a setting for this run only (the settings themselves are kept in\n"
    "the user data directory, and changed in the launcher or the game's menu):\n"
    "  --scale N             start with the screens N times their size (1 to 8)\n"
    "  --side-by-side        the screens side by side instead of one above the other\n"
    "  --gap N               N DS pixels between the screens (0 to 96)\n"
    "  --touch-screen-first  the touch screen above (or left of) the other\n"
    "  --fit                 fill the window instead of scaling by whole numbers\n"
    "  --fullscreen          start full screen\n"
    "  --no-vsync            do not wait for the screen's refresh to show a picture\n"
    "  --textures MODE       3D scenes' textures: sharp (nearest, as on the DS) or\n"
    "                        smooth (bilinear); the HUD and 2D always stay sharp\n"
    "  --log-level LEVEL     error, warning, info or debug\n"
    "\n"
    "Other options:\n"
    "  --no-settings         use the default settings, and do not keep any changes\n"
    "  --log-to-stderr       also write the log to the standard error stream\n"
    "  --unthrottled         run as fast as possible instead of at the console's rate\n"
    "  --frames N            stop after N frames\n"
    "  --hold BUTTON@F[-L]   hold a DS button from frame F to frame L (counting from 1),\n"
    "                        as if pressed; may be given more than once\n"
    "  --touch X,Y@F[-L]     touch the touch screen at pixel X,Y from frame F to frame L;\n"
    "                        may be given more than once\n"
    "  --capture FILE        with --frames, write the window's last frame to FILE (BMP)\n"
    "  --interpret-only      do not use the translated code (bright-dawn-game only)\n"
    "  --video-drivers       list the window systems this build can use, and stop\n"
    "  --version             print the version, and stop\n"
    "\n"
    "Keyboard: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start,\n"
    "Right Shift = Select. Controllers: by position, as on the DS. The left mouse\n"
    "button on the touch screen is the stylus. Escape (or a controller's guide\n"
    "button): the menu. F11 or Alt+Enter: full screen. Tab: stacked or side by side.\n"
    "F12: whole-number scaling or fill.\n";

struct CommandLine {
  app::Options options;
  bool have_source = false;
  std::vector<std::pair<std::string_view, std::string>> overrides;
  bool no_settings = false;
  bool log_to_stderr = false;
};

bool parse_number(const char* text, long long low, long long high, long long& out) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0' || value < low || value > high) return false;
  out = value;
  return true;
}

std::string version_text(int version) {
  return std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
         std::to_string(SDL_VERSIONNUM_MICRO(version));
}

// The log's file in the user data directory, and its first lines: what is
// running, on what, with which settings.
void start_log(const App& a, const CommandLine& cl, int argc, char** argv,
               const std::vector<std::string>& settings_problems, const std::string& data_problem) {
  const auto level = log::level_named(a.settings.choice(ui::key::kLogLevel));
  log::set_level(level.value_or(log::Level::Info));
  log::set_standard_error(cl.log_to_stderr);
  std::string log_problem;
  if (!a.user_data.empty()) {
    const std::filesystem::path folder = a.user_data / "logs";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = log::new_file_name(folder, "bright-dawn-");
    if (Status opened = log::open_file(file); !opened) {
      log_problem = opened.error().message;
    } else {
      log::remove_old_files(folder, "bright-dawn-", static_cast<std::size_t>(a.settings.number(ui::key::kLogsKept)));
      platform::log_crashes();
    }
  }
  log::info("start", std::string("Bright Dawn ") + BD_VERSION + (kHasTranslation ? ", the built game" : ""));
  std::string line;
  for (int i = 0; i < argc; ++i) line += (i == 0 ? "" : " ") + std::string(argv[i]);
  log::info("start", "command line: " + line);
  if (!data_problem.empty()) log::warning("start", "no user data directory: " + data_problem);
  else log::info("start", "user data: " + app::utf8(a.user_data));
  if (!log_problem.empty()) log::warning("start", log_problem);
  if (a.settings_path.empty()) {
    log::info("settings", "the defaults, not kept");
  } else {
    log::info("settings", "from " + app::utf8(a.settings_path));
    for (const std::string& p : settings_problems) log::warning("settings", p);
  }
  for (const platform::SettingDef& def : a.settings.definitions())
    if (!a.settings.is_default(def.key))
      log::info("settings", def.key + " = " + a.settings.value(def.key) +
                                (a.settings.overridden(def.key) ? " (for this run)" : ""));
}

void log_system(SDL_Renderer* renderer) {
  log::info("system", std::string(SDL_GetPlatform()) + ", " + std::to_string(SDL_GetNumLogicalCPUCores()) +
                          " processors, " + std::to_string(SDL_GetSystemRAM()) + " MB of memory");
  log::info("system", "SDL " + version_text(SDL_GetVersion()) + ", video: " +
                          (SDL_GetCurrentVideoDriver() != nullptr ? SDL_GetCurrentVideoDriver() : "none") +
                          ", renderer: " + (renderer != nullptr ? SDL_GetRendererName(renderer) : "none"));
  int count = 0;
  if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
    for (int i = 0; i < count; ++i) {
      const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[i]);
      if (mode == nullptr) continue;
      log::writef(log::Level::Info, "system", "display %d: %d by %d at %.2f Hz, scale %.2f", i + 1, mode->w, mode->h,
                  static_cast<double>(mode->refresh_rate), static_cast<double>(SDL_GetDisplayContentScale(displays[i])));
    }
    SDL_free(displays);
  }
}

// The launcher's size: generous, within the display.
void size_for_launcher(SDL_Window* window) {
  if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) SDL_SetWindowFullscreen(window, false);
  int w = 1100, h = 720;
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
    w = std::min(w, usable.w * 9 / 10);
    h = std::min(h, usable.h * 9 / 10);
  }
  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

struct Played {
  app::GameEnd end = app::GameEnd::Quit;
  int status = 0;
};

// Plays the game from `source` until the run ends.
Played play(App& a, const std::filesystem::path& source, bool can_go_back) {
  Played played;
  std::error_code ec;
  Result<nds::Program> loaded = Error{ErrorCode::Io, "not loaded"};
  Result<rom::Rom> rom = Error{ErrorCode::Io, "no ROM"};
  log::info("game", "loading " + app::utf8(source));
  if (std::filesystem::is_directory(source, ec)) {
    loaded = nds::program_from_directory(source);
  } else {
    rom = rom::Rom::open(source);
    if (!rom) {
      complain(a.window, rom.error().message);
      played.status = 1;
      return played;
    }
    if (auto status = rom::require_dark_dawn(rom.value().header()); !status) {
      complain(a.window, status.error().message);
      played.status = 1;
      return played;
    }
    const rom::KnownTitle* title = rom::identify_title(rom.value().header());
    log::writef(log::Level::Info, "game", "Golden Sun: Dark Dawn, %s, %.*s, %zu bytes", rom::to_string(title->region),
                static_cast<int>(title->game_code.size()), title->game_code.data(), rom.value().bytes().size());
    loaded = nds::program_from_rom(rom.value());
  }
  if (!loaded) {
    complain(a.window, loaded.error().message);
    played.status = 1;
    return played;
  }

  const TranslatedEntry* translation = a.options.interpret_only ? kNoTranslations : a.translation;
  std::size_t translated_functions = 0;
  while (translation[translated_functions].function != nullptr) ++translated_functions;
  std::printf("%zu translated functions in use\n", translated_functions);
  std::fflush(stdout);
  log::writef(log::Level::Info, "game", "%zu translated functions in use%s", translated_functions,
              translated_functions == 0 ? ": every instruction is interpreted" : "");
  nds::Console console(loaded.value(), translation);
  if (rom) console.insert_cartridge(rom.value().bytes());
  if (auto status = console.direct_boot(); !status) {
    complain(a.window, status.error().message);
    played.status = 1;
    return played;
  }

  app::GameScreen screen(a, console, can_go_back);
  std::string error;
  if (!screen.open(error)) {
    complain(a.window, error);
    played.status = 1;
    return played;
  }
  log::info("game", "started");
  console.on_frame = [&screen] { return screen.frame(); };
  const nds::RunResult result = console.run(0, a.options.frames);
  console.on_frame = nullptr;
  const std::uint64_t frames = console.display().statistics().frames;
  std::printf("%" PRIu64 " frames shown\n", frames);
  const runtime::Statistics& stats = console.machine().statistics();
  log::writef(log::Level::Info, "game",
              "ended after %" PRIu64 " frames: %" PRIu64 " instructions interpreted, %zu places interpreted, %" PRIu64
              " interrupts taken, %" PRIu64 " sleeps",
              frames, stats.interpreted_instructions, stats.interpreter_entries.size(), stats.interrupts_taken,
              console.sleeps());
  for (const auto& [what, count] : console.polygon_renderer().notes())
    log::writef(log::Level::Info, "game", "asked of the 3D hardware and not done: %s (%" PRIu64 " frames)", what.c_str(),
                count);
  for (const auto& [number, count] : console.software_interrupts())
    log::writef(log::Level::Debug, "game", "BIOS call 0x%02" PRIX32 ": %" PRIu64 " times", number, count);
  played.end = screen.end() == app::GameEnd::Running ? app::GameEnd::Quit : screen.end();
  switch (result.reason) {
    case nds::StopReason::Unhandled: {
      char where[16];
      std::snprintf(where, sizeof where, "0x%08" PRIX32, result.address);
      complain(a.window, std::string("The game stopped at ") + where + ": it needed something Bright Dawn "
                             "does not do yet.\n\n" + result.detail);
      played.status = 2;
      if (can_go_back) played.end = app::GameEnd::ToLauncher;
      break;
    }
    case nds::StopReason::Returned:
      complain(a.window, "The game's program ended.");
      played.status = 2;
      if (can_go_back) played.end = app::GameEnd::ToLauncher;
      break;
    case nds::StopReason::FrameLimit: log::info("game", "the frames asked for were run"); break;
    case nds::StopReason::Asked: break;
    case nds::StopReason::InstructionLimit: log::info("game", "the instructions asked for were run"); break;
  }
  if (!a.options.capture.empty() && !screen.captured()) played.status = 1;
  return played;
}

int run(CommandLine& cl, int argc, char** argv) {
  App a{platform::Settings(ui::game_setting_definitions())};
  a.options = cl.options;
  a.translation = kTranslation;
  a.translated_build = kHasTranslation;

  std::string data_problem;
  if (Result<std::filesystem::path> data = platform::user_data_dir(); data) a.user_data = data.value();
  else data_problem = data.error().message;
  std::vector<std::string> settings_problems;
  if (!cl.no_settings && !a.user_data.empty()) {
    a.settings_path = ui::settings_file(a.user_data);
    Result<std::vector<std::string>> loaded = a.settings.load(a.settings_path);
    if (loaded) settings_problems = loaded.value();
    else settings_problems.push_back(loaded.error().message);
  }
  for (const auto& [key, value] : cl.overrides) {
    if (Status s = a.settings.set_for_run(key, value); !s) {
      complain(nullptr, s.error().message + "\n\n" + kUsage);
      return 1;
    }
  }
  start_log(a, cl, argc, argv, settings_problems, data_problem);

  SDL_SetAppMetadata("Bright Dawn", BD_VERSION, nullptr);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    complain(nullptr, std::string("Cannot start SDL: ") + SDL_GetError());
    return 1;
  }
  int status = 0;
  {
    const gfx::Point size = cl.have_source ? gfx::window_size(static_cast<int>(a.settings.number(ui::key::kScale)),
                                                             ui::layout_options(a.settings))
                                           : gfx::Point{1100, 720};
    if (!SDL_CreateWindowAndRenderer("Bright Dawn", size.x, size.y, SDL_WINDOW_RESIZABLE, &a.window, &a.renderer)) {
      complain(nullptr, std::string("Cannot open a window: ") + SDL_GetError());
      SDL_Quit();
      return 1;
    }
    SDL_SetWindowMinimumSize(a.window, gfx::kScreenWidth / 2, gfx::kScreenHeight / 2);
    log_system(a.renderer);
    std::string error;
    if (!a.gui.init(a.window, a.renderer, error)) {
      complain(a.window, error);
      status = 1;
    } else {
      std::filesystem::path source = cl.have_source ? cl.options.source : std::filesystem::path();
      const bool from_launcher = !cl.have_source;
      if (from_launcher) size_for_launcher(a.window);
      for (;;) {
        if (source.empty()) {
          app::LauncherChoice choice;
          {
            app::Launcher launcher(a);
            choice = launcher.run();
          }
          if (choice.kind == app::LauncherChoice::Quit) break;
          source = choice.rom;
        }
        const Played played = play(a, source, from_launcher);
        status = played.status;
        if (played.end != app::GameEnd::ToLauncher) break;
        source.clear();
        status = 0;
        size_for_launcher(a.window);
      }
    }
  }
  // The program's screens go before the renderer they draw with.
  a.gui.shutdown();
  SDL_DestroyRenderer(a.renderer);
  SDL_DestroyWindow(a.window);
  SDL_Quit();
  log::writef(log::Level::Info, "start", "the end, with status %d", status);
  log::close_file();
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  CommandLine cl;
  app::Options& options = cl.options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    long long number = 0;
    const bool has_value = i + 1 < argc;
    if (arg == "--version") {
      std::printf("bright-dawn %s\n", BD_VERSION);
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      std::printf("%s", kUsage);
      return 0;
    } else if (arg == "--video-drivers") {
      for (int d = 0; d < SDL_GetNumVideoDrivers(); ++d) std::printf("%s\n", SDL_GetVideoDriver(d));
      return 0;
    } else if (arg == "--scale" && has_value && parse_number(argv[i + 1], 1, 8, number)) {
      cl.overrides.emplace_back(ui::key::kScale, argv[++i]);
    } else if (arg == "--gap" && has_value && parse_number(argv[i + 1], 0, 96, number)) {
      cl.overrides.emplace_back(ui::key::kGap, argv[++i]);
    } else if (arg == "--textures" && has_value) {
      cl.overrides.emplace_back(ui::key::kTextures, argv[++i]);
    } else if (arg == "--log-level" && has_value) {
      cl.overrides.emplace_back(ui::key::kLogLevel, argv[++i]);
    } else if (arg == "--frames" && has_value && parse_number(argv[i + 1], 1, 1LL << 40, number)) {
      options.frames = static_cast<std::uint64_t>(number);
      ++i;
    } else if (arg == "--hold" && has_value) {
      const auto press = input::parse_scripted_press(argv[i + 1]);
      if (!press) {
        complain(nullptr, std::string("Not a button and frames: ") + argv[i + 1] + "\n\n" + kUsage);
        return 1;
      }
      options.script.push_back(*press);
      ++i;
    } else if (arg == "--touch" && has_value) {
      const auto touch = input::parse_scripted_touch(argv[i + 1]);
      if (!touch) {
        complain(nullptr, std::string("Not a point and frames: ") + argv[i + 1] + "\n\n" + kUsage);
        return 1;
      }
      options.touches.push_back(*touch);
      ++i;
    } else if (arg == "--capture" && has_value) {
      options.capture = argv[++i];
    } else if (arg == "--side-by-side") {
      cl.overrides.emplace_back(ui::key::kArrangement, "side_by_side");
    } else if (arg == "--touch-screen-first") {
      cl.overrides.emplace_back(ui::key::kTouchScreenFirst, "on");
    } else if (arg == "--fit") {
      cl.overrides.emplace_back(ui::key::kScaling, "fill");
    } else if (arg == "--fullscreen") {
      cl.overrides.emplace_back(ui::key::kFullscreen, "on");
    } else if (arg == "--no-vsync") {
      cl.overrides.emplace_back(ui::key::kVsync, "off");
    } else if (arg == "--no-settings") {
      cl.no_settings = true;
    } else if (arg == "--log-to-stderr") {
      cl.log_to_stderr = true;
    } else if (arg == "--interpret-only") {
      options.interpret_only = true;
    } else if (arg == "--unthrottled") {
      options.unthrottled = true;
    } else if (!arg.empty() && arg[0] != '-' && !cl.have_source) {
      // Command-line arguments arrive as UTF-8 (SDL_main sees to that on
      // Windows).
      options.source = app::from_utf8(argv[i]);
      cl.have_source = true;
    } else {
      complain(nullptr, "Not understood: " + arg + "\n\n" + kUsage);
      return 1;
    }
  }
  if (!options.capture.empty() && options.frames == 0) {
    complain(nullptr, "--capture needs --frames: it writes the last of them.");
    return 1;
  }
  if (options.frames != 0 && !cl.have_source) {
    complain(nullptr, "--frames needs a ROM: it is for playing one at once.");
    return 1;
  }
  return run(cl, argc, argv);
}
