// The game: both DS screens in the window, a frame at a time, at the
// console's rate. The keyboard and any number of game controllers press the
// DS's buttons; the left mouse button on the touch screen is the stylus.
//
// The game's step and the pictures are kept apart (platform/frame_clock.h).
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
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/hardware_3d.h"
#include "app/settings_book.h"
#include "gfx/screen_layout.h"
#include "nds/console.h"
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
  // At each frame (Console::on_frame): the screens shown, the input read,
  // time kept. False when the run is to end; end() says why.
  bool frame();
  GameEnd end() const { return end_; }
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
  // The screens, enlarged to a whole multiple for SharpSmoothEdges, made
  // again when the multiple changes or the screens do.
  void prepare_enlarged(int factor);
  void draw();
  void capture();
  void count_rates(bool new_frame, bool shown);
  // The pause menu, until it is closed. False if the run is to end.
  bool run_menu();
  void draw_menu();

  App& app_;
  nds::Console& console_;
  const bool can_go_back_;
  gfx::LayoutOptions layout_;
  gfx::ScreenLayout screens_;
  SDL_Texture* top_ = nullptr;
  SDL_Texture* bottom_ = nullptr;
  gfx::ScreenFilter filter_ = gfx::ScreenFilter::SharpSmoothEdges;
  std::array<SDL_Texture*, 2> enlarged_{};  // Render targets: top, bottom.
  int enlarged_factor_ = 0;
  bool enlarged_stale_ = true;
  bool enlarging_failed_ = false;
  int largest_texture_ = 4096;
  // The 3D scenes by the graphics hardware (Settings, Graphics): tried once
  // when first asked for; the main engine's screen as it made it, for the
  // frame last shown, or null when the frame was drawn here.
  Hardware3D hardware_;
  bool hardware_tried_ = false, hardware_failed_ = false;
  SDL_Texture* main_3d_ = nullptr;
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
};

}  // namespace bd::app
