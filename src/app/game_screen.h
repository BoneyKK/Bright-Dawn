// The game: both DS screens in the window, a frame at a time, at the
// console's rate. The keyboard and any number of game controllers press the
// DS's buttons; the left mouse button on the touch screen is the stylus.
//
// The game's step and the pictures are kept apart (platform/frame_clock.h).
// In a new game, Start skips the long story at the start, up to the title
// (game/opening_skip.h), unless switched off in Settings, Quality of life.
// On the field, a controller's left stick walks at any angle and pace
// (game/stick_walk.h), unless switched off in Settings, Controls; elsewhere
// it is the d-pad.
// Escape, or a controller's guide button, pauses the game and opens the
// menu: continue, the settings book, back to the launcher, quit. Settings
// changed there take effect at once. Keys of the window itself: F11 or
// Alt+Enter, full screen; Tab, the screens one above the other or side by
// side; F12, whole-number scaling or filling the window. These change the
// settings too, so they are kept.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/audio_out.h"
#include "app/build_tools.h"
#include "debug/frame_log_file.h"
#include "debug/frame_stepper.h"
#include "debug/gpu_info.h"
#include "debug/perf_report.h"
#include "game/opening_skip.h"
#include "game/stick_walk.h"
#include "app/hardware_3d.h"
#include "app/settings_book.h"
#include "ui/game_settings.h"
#include "gfx/screen_layout.h"
#include "nds/console.h"
#include "platform/audio_feed.h"
#include "platform/frame_clock.h"

namespace bd::app {

enum class GameEnd {
  Running,     // Not ended.
  Quit,        // The window was closed, or Quit chosen.
  ToLauncher,  // Back to the launcher was chosen.
};

class GameScreen {
 public:
  // `can_go_back`: the launcher is there to go back to.
  GameScreen(App& app, nds::Console& console, bool can_go_back);
  GameScreen(const GameScreen&) = delete;
  GameScreen& operator=(const GameScreen&) = delete;
  ~GameScreen();

  bool open(std::string& error);
  // After the console is reset: the one started anew, in place of the one
  // before (which may then go). detach() lets go of it.
  void attach(nds::Console& console);
  void detach();
  // At each frame (Console::on_frame): the screens shown, the input read,
  // time kept. False when the run is to end; end() says why.
  bool frame();
  GameEnd end() const { return end_; }
  // The frame log's row for the frame just run (Console::on_frame), when
  // the log is on (debug/frame_log.h).
  void log_frame();
  // After the frame's row (Console::on_frame): while the frame stepper
  // holds the game (debug/frame_stepper.h), the window is redrawn and the
  // hotkeys read, and nothing of the game runs. False if the run is to end.
  bool hold_if_paused();
  bool captured() const { return captured_; }

 private:
  struct Key {
    SDL_Scancode code;
    std::uint32_t bit;
  };
  struct PadButton {
    SDL_GamepadButton button;
    std::uint32_t bit;
  };
  enum class MenuPage { Main, Settings };

  void apply_settings(bool first);
  bool handle_events();
  bool handle_window_key(const SDL_KeyboardEvent& key);
  // The touch screen alone, or back to the arrangement before (a hotkey).
  void switch_bottom_screen_only();
  void add_gamepad(SDL_JoystickID id);
  void remove_gamepad(SDL_JoystickID id);
  void read_input();
  void upload_screens();
  // The graphics hardware's capture of the frame just run, if it made one.
  void keep_capture();
  // --check-2d: the 2D picture the graphics hardware drew of screen `i`
  // against the processor's.
  void check_2d(int i, bool with_marks);
  // The scale of the graphics hardware's 3D for the screens as laid out.
  int scale_3d() const;
  // The processor draws the 3D pictures from the next frame on.
  void give_up_hardware(const char* why);
  // Screen i's picture `source` (the console's or the graphics hardware's),
  // enlarged without smoothing to `factor` times the DS screen's size, made
  // again when the factor changes or the screens do; nothing if it cannot be.
  // `source` (made at `made_at` times the DS's resolution) enlarged `factor`
  // / `made_at` times, for screen `i`.
  SDL_Texture* enlarged(int i, SDL_Texture* source, int made_at, int factor);
  // The screen the main engine shows (0 the top, 1 the touch screen): the
  // one that is wide with widescreen on.
  int main_screen() const;
  void draw();
  void capture(bool last);
  // Writes the window's picture as a BMP file; false (logged) if it cannot.
  bool write_window(const std::filesystem::path& file);
  // The debug hotkeys (launcher, Debug page), when bound and on: true if
  // `code` or `button` was one.
  bool debug_hotkey(SDL_Scancode code, SDL_GamepadButton button);
  // The frame overlay (launcher, Debug page), over the window's corner:
  // never into the game's screens.
  void draw_overlay();
  // Capture this frame: the picture shown now, and its frame-log row (now,
  // or once log_frame() has it).
  void capture_this_frame();
  void write_capture_row(const std::filesystem::path& file);
  // The performance report's file, at the end (debug/perf_report.h).
  void write_perf_report();
  // The graphics card and the known issues it matches: logged, kept for the
  // report, and their workarounds applied unless switched off.
  void describe_gpu();
  // Writes the last 3D scene, as made, to --dump-3d's file.
  void dump_3d();
  void count_rates(bool new_frame, bool shown);
  // The pause menu, until it is closed. False if the run is to end.
  bool run_menu();
  void draw_menu();

  App& app_;
  nds::Console* console_;  // Null once detached.
  const bool can_go_back_;
  gfx::LayoutOptions layout_;
  gfx::ScreenLayout screens_;
  SDL_Texture* top_ = nullptr;
  SDL_Texture* bottom_ = nullptr;
  gfx::ScreenFilter filter_ = gfx::ScreenFilter::SharpSmoothEdges;
  std::array<SDL_Texture*, 2> enlarged_{};  // Render targets: top, bottom.
  std::array<int, 2> enlarged_factor_{};
  std::array<int, 2> enlarged_width_{};
  std::array<bool, 2> enlarged_stale_{true, true};
  bool enlarging_failed_ = false;
  int largest_texture_ = 4096;
  // The 3D scenes by the graphics hardware (Settings, Graphics): tried once
  // when first asked for; the main engine's screen as it made it, for the
  // frame last shown, or null when the frame was drawn here.
  Hardware3D hardware_;
  bool hardware_tried_ = false, hardware_failed_ = false;
  bool wide_ = false;  // Widescreen, with the graphics hardware (Hardware3D::set_wide).
  bool wide_hud_ = false;  // And its HUD at the wide screen's edges (nds::WideHud).
  // The screen the main engine was last on, and for how many pictures in a
  // row: the wide one only once it stays (the 3D on both screens, as the
  // title has it, switches it every frame).
  int main_screen_seen_ = -1, main_screen_pictures_ = 0;
  std::uint64_t checked_2d_ = 0, differed_2d_ = 0;  // --check-2d: pictures checked, and those that differ.
  std::string hardware_problem_;  // Why the graphics hardware cannot draw, when it is asked for.
  // The screens as the graphics hardware drew them (top, bottom), where it
  // did, and the last capture it made.
  std::array<SDL_Texture*, 2> shown_3d_{};
  std::uint64_t capture_kept_ = 0;
  // The game's sound (Settings, Sound), a frame at a time.
  platform::AudioFeed sound_;
  AudioOut audio_;
  std::vector<SDL_Gamepad*> gamepads_;
  std::vector<Key> keys_;
  std::vector<PadButton> pad_buttons_;
  std::set<std::string> unknown_controls_;  // Reported in the log already.
  SDL_Scancode bottom_only_key_ = SDL_SCANCODE_UNKNOWN;
  SDL_GamepadButton bottom_only_button_ = SDL_GAMEPAD_BUTTON_INVALID;
  std::array<std::uint8_t, gfx::kScreenWidth * gfx::kScreenHeight * 4> pixels_{};
  bool touching_ = false;
  gfx::Point touch_at_;
  platform::FrameClock clock_;
  bool vsync_ = false;
  std::uint64_t settings_seen_ = 0;
  long long scale_applied_ = 0;
  bool fullscreen_applied_ = false;
  std::string title_;
  std::uint64_t frames_run_ = 0, frames_this_second_ = 0, pictures_this_second_ = 0, second_started_ = 0;
  std::uint64_t rates_logged_ = 0, drops_logged_ = 0, drops_logged_at_ = 0;
  bool captured_ = false;
  bool menu_open_ = false;
  MenuPage menu_page_ = MenuPage::Main;
  SettingsBook book_;
  GameEnd end_ = GameEnd::Running;
  // Start skips the story at the start of a new game, up to the title
  // (Settings, Quality of life).
  game::OpeningSkip opening_skip_;
  bool was_skipping_ = false;
  game::StickWalk stick_walk_;
  game::StickWalk::Mode stick_mode_ = game::StickWalk::Mode::Idle;
  // The frame log, when it is on (debug/frame_log.h).
  std::unique_ptr<frame_log::File> frame_log_;
  std::uint64_t log_executed_ = 0, log_interpreted_ = 0;
  // The frame log's row of the last frame: names and values, whether or not
  // the file is written (the overlay, the capture and the report use it).
  std::vector<std::string> row_names_;
  std::vector<double> row_values_;
  bool captured_this_frame_ = false;
  // Debug (launcher, Debug page; debug/frame_stepper.h).
  frame_log::FrameStepper stepper_;
  bool pause_step_ = false, overlay_allowed_ = false, overlay_shown_ = false, capture_hotkey_ = false;
  bool capture_asked_ = false;
  std::filesystem::path capture_row_pending_;  // Written by log_frame() once the row is there.
  struct DebugKey {
    ui::Hotkey hotkey;
    SDL_Scancode code;
    SDL_GamepadButton button;
  };
  std::vector<DebugKey> debug_keys_;
  // The performance report, when asked for, and where it goes.
  std::unique_ptr<debug::PerfReport> perf_;
  std::filesystem::path perf_path_;
  // The graphics card, the known issues it matches, and the workarounds
  // applied (debug/gpu_info.h).
  debug::GpuDescription gpu_;
  std::vector<const debug::GpuIssue*> gpu_issues_;
  std::uint32_t workarounds_ = 0;
  double ticks_per_us_ = 1;      // The frame log's clock, when it runs without its file.
  std::string vsync_mode_ = "off";
};

}  // namespace bd::app
