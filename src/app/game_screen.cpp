#include "app/game_screen.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/version.h"
#include "gfx/screen_image.h"
#include "imgui.h"
#include "ui/game_settings.h"

namespace bd::app {

namespace key = ui::key;

GameScreen::GameScreen(App& app, nds::Console& console, bool can_go_back)
    : app_(app), console_(console), can_go_back_(can_go_back) {}

GameScreen::~GameScreen() {
  console_.sound_hardware().on_output = nullptr;
  for (SDL_Gamepad* pad : gamepads_) SDL_CloseGamepad(pad);
  if (top_ != nullptr) SDL_DestroyTexture(top_);
  if (bottom_ != nullptr) SDL_DestroyTexture(bottom_);
  for (SDL_Texture* t : enlarged_)
    if (t != nullptr) SDL_DestroyTexture(t);
}

bool GameScreen::open(std::string& error) {
  for (SDL_Texture** texture : {&top_, &bottom_}) {
    *texture = SDL_CreateTexture(app_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                 gfx::kScreenWidth, gfx::kScreenHeight);
    if (*texture == nullptr) {
      error = std::string("Cannot make a texture for the screens: ") + SDL_GetError();
      return false;
    }
    // Opaque: drawn over what is there, not blended.
    SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_NONE);
  }
  console_.sound_hardware().on_output = [this](nds::SoundHardware::Output o) { sound_.add(o.left, o.right); };
  if (!app_.options.record_sound.empty()) {
    std::string why;
    if (sound_.record(app_.options.record_sound, why))
      log::info("sound", "the game's sound is kept in " + app_.options.record_sound.string());
    else
      log::warning("sound", why);
  }
  largest_texture_ = static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(app_.renderer),
                                                            SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
  // Controllers already there: the launcher saw them come.
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) add_gamepad(ids[i]);
    SDL_free(ids);
  }
  // Which code runs: linked in, the game built from the player's ROM, or
  // every instruction interpreted (play() decides and sets native_code).
  title_ = std::string("Bright Dawn ") + BD_VERSION +
           (!app_.native_code          ? " (interpreted)"
            : app_.translated_build    ? " (translated)"
                                       : " (built game)");
  SDL_SetWindowTitle(app_.window, title_.c_str());
  apply_settings(true);
  second_started_ = SDL_GetTicksNS();
  clock_.tick(second_started_);
  return true;
}

void GameScreen::apply_settings(bool first) {
  const platform::Settings& s = app_.settings;
  settings_seen_ = s.revision();
  // The bindings (Settings, Controls), named as SDL names keys and
  // controller buttons. A name it does not know is reported once.
  keys_.clear();
  pad_buttons_.clear();
  for (const input::Binding& b : ui::keyboard_bindings(s)) {
    const SDL_Scancode code = SDL_GetScancodeFromName(b.control.c_str());
    if (code != SDL_SCANCODE_UNKNOWN) keys_.push_back({code, input::bit(b.button)});
    else if (unknown_controls_.insert(b.control).second) log::warning("input", "no key is called \"" + b.control + "\"");
  }
  for (const input::Binding& b : ui::controller_bindings(s)) {
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(b.control.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) pad_buttons_.push_back({button, input::bit(b.button)});
    else if (unknown_controls_.insert(b.control).second)
      log::warning("input", "no controller button is called \"" + b.control + "\"");
  }
  const std::string bottom_key = ui::hotkey_binding(s, ui::Hotkey::BottomScreenOnly, true);
  const std::string bottom_button = ui::hotkey_binding(s, ui::Hotkey::BottomScreenOnly, false);
  bottom_only_key_ = bottom_key.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(bottom_key.c_str());
  bottom_only_button_ =
      bottom_button.empty() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(bottom_button.c_str());
  layout_ = ui::layout_options(s);
  // Screen filtering (gfx::ScreenFilter). The screens' own textures are
  // scaled nearest, except for Smooth; for SharpSmoothEdges they are first
  // enlarged into render targets, which are then scaled bilinear. Both
  // steps are done by the graphics hardware as it draws.
  filter_ = ui::screen_filter(s);
  for (SDL_Texture* texture : {top_, bottom_})
    SDL_SetTextureScaleMode(texture, filter_ == gfx::ScreenFilter::Smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
  enlarged_stale_ = true;
  // The 3D scenes: by the graphics hardware where it can, else here.
  bool hardware = ui::hardware_3d(s);
  if (hardware && !hardware_tried_) {
    hardware_tried_ = true;
    std::string why;
    if (!hardware_.open(app_.renderer, why))
      log::warning("3d", "the graphics hardware cannot draw 3D scenes, so the processor draws them: " + why);
  }
  hardware = hardware && hardware_.is_open() && !hardware_failed_;
  console_.display().set_hardware_3d(hardware);
  audio_.set(s.toggle(key::kSound), static_cast<int>(s.number(key::kVolume)));
  if (!hardware) shown_3d_ = {};
  const bool smooth = ui::smooth_textures(s);
  console_.polygon_renderer().set_texture_filter(smooth ? nds::PolygonRenderer::TextureFilter::Bilinear
                                                        : nds::PolygonRenderer::TextureFilter::Nearest);
  // Vsync: adaptive where the system has it (a late picture is shown at
  // once rather than a whole refresh later), else plain. Without it the
  // window waits for the next step itself.
  const bool want_vsync = s.toggle(key::kVsync);
  if (first || want_vsync != vsync_) {
    vsync_ = want_vsync && (SDL_SetRenderVSync(app_.renderer, SDL_RENDERER_VSYNC_ADAPTIVE) ||
                            SDL_SetRenderVSync(app_.renderer, 1));
    if (!want_vsync) SDL_SetRenderVSync(app_.renderer, 0);
    log::info("window", std::string("vsync ") + (vsync_ ? "on" : want_vsync ? "asked for, but not available" : "off"));
  }
  const bool full = s.toggle(key::kFullscreen);
  if (first || full != fullscreen_applied_) {
    fullscreen_applied_ = full;
    if (!first || full) {
      SDL_SetWindowFullscreen(app_.window, full);
      log::info("window", full ? "full screen" : "in a window");
    }
  }
  const long long scale = s.number(key::kScale);
  if (first || scale != scale_applied_) {
    scale_applied_ = scale;
    if ((SDL_GetWindowFlags(app_.window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) == 0) {
      const gfx::Point size = gfx::window_size(static_cast<int>(scale), layout_);
      SDL_SetWindowSize(app_.window, size.x, size.y);
    }
  }
  if (first) {
    log::writef(log::Level::Info, "game", "screens %s%s, %s, filter %s, gap %d; 3D textures %s",
                layout_.arrangement == gfx::Arrangement::Stacked      ? "one above the other"
                : layout_.arrangement == gfx::Arrangement::SideBySide ? "side by side"
                                                                      : "the touch screen alone",
                layout_.bottom_first ? ", touch screen first" : "",
                layout_.scaling == gfx::Scaling::WholeNumbers ? "whole-number scaling" : "filling the window",
                filter_ == gfx::ScreenFilter::SharpSmoothEdges ? "sharp pixels, smooth edges"
                : filter_ == gfx::ScreenFilter::Smooth         ? "smooth"
                                                               : "sharp",
                layout_.gap, smooth ? "smooth" : "sharp");
  }
}

bool GameScreen::frame() {
  ++frames_run_;
  audio_.play(sound_.take());
  keep_capture();
  if (!handle_events()) return false;
  if (menu_open_ && !run_menu()) return false;
  if (app_.settings.revision() != settings_seen_) apply_settings(false);
  read_input();
  const bool capture_last = app_.options.frames != 0 && frames_run_ == app_.options.frames && !app_.options.capture.empty();
  const bool capture_now = capture_last || (app_.options.capture_every != 0 && !app_.options.capture.empty() &&
                                            frames_run_ % app_.options.capture_every == 0);
  const bool show = app_.options.unthrottled || clock_.frame_done(SDL_GetTicksNS());
  if (clock_.drops() != drops_logged_) {
    const std::uint64_t now = SDL_GetTicksNS();
    if (drops_logged_at_ == 0 || now - drops_logged_at_ >= 10'000'000'000ull) {
      log::writef(log::Level::Info, "game",
                  "behind real time: the game runs slower than the console would (time dropped %" PRIu64
                  " times so far)",
                  clock_.drops());
      drops_logged_ = clock_.drops();
      drops_logged_at_ = now;
    }
  }
  if (!show && !capture_now) {
    count_rates(true, false);
    return true;
  }
  upload_screens();
  for (bool first = true;; first = false) {
    draw();
    if (first && capture_now) capture(capture_last);
    SDL_RenderPresent(app_.renderer);
    count_rates(first, true);
    if (app_.options.unthrottled) break;
    if (!vsync_) {
      const std::uint64_t wait = clock_.until_due();
      if (wait != 0) SDL_DelayPrecise(wait);
    }
    clock_.tick(SDL_GetTicksNS());
    if (clock_.step_due()) break;
    // Waiting for the next step: the window stays responsive.
    if (!handle_events()) return false;
    if (menu_open_) break;
  }
  return true;
}

void GameScreen::switch_bottom_screen_only() {
  for (const auto& [key, value] : ui::bottom_screen_switch(app_.settings)) app_.set_setting(key, value);
}

bool GameScreen::handle_window_key(const SDL_KeyboardEvent& k) {
  if (k.scancode != SDL_SCANCODE_UNKNOWN && k.scancode == bottom_only_key_) {
    switch_bottom_screen_only();
    return true;
  }
  if (k.scancode == SDL_SCANCODE_F11 || (k.scancode == SDL_SCANCODE_RETURN && (k.mod & SDL_KMOD_ALT) != 0)) {
    app_.set_setting(key::kFullscreen, app_.settings.toggle(key::kFullscreen) ? "off" : "on");
    return true;
  }
  if (k.scancode == SDL_SCANCODE_TAB) {
    app_.set_setting(key::kArrangement,
                     app_.settings.choice(key::kArrangement) == "stacked" ? "side_by_side" : "stacked");
    return true;
  }
  if (k.scancode == SDL_SCANCODE_F12) {
    app_.set_setting(key::kScaling, app_.settings.choice(key::kScaling) == "whole" ? "fill" : "whole");
    return true;
  }
  return false;
}

bool GameScreen::handle_events() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
        log::info("window", "closed");
        end_ = GameEnd::Quit;
        return false;
      case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) break;
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
          menu_open_ = true;
        } else {
          handle_window_key(event.key);
        }
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) menu_open_ = true;
        else if (bottom_only_button_ != SDL_GAMEPAD_BUTTON_INVALID && event.gbutton.button == bottom_only_button_)
          switch_bottom_screen_only();
        break;
      case SDL_EVENT_GAMEPAD_ADDED:
        add_gamepad(event.gdevice.which);
        break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(event.gdevice.which);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        log::debug("window", "lost the keyboard");
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        log::debug("window", "has the keyboard");
        break;
      case SDL_EVENT_RENDER_TARGETS_RESET:
        // The enlarged screens' contents were lost: made again.
        enlarged_stale_ = true;
        break;
      case SDL_EVENT_RENDER_DEVICE_RESET:
        log::warning("window", "the graphics device was reset");
        enlarged_stale_ = true;
        break;
      case SDL_EVENT_WINDOW_RESIZED:
        log::writef(log::Level::Debug, "window", "%d by %d", event.window.data1, event.window.data2);
        break;
      default:
        break;
    }
    // The program's screens follow the display's scale even while hidden.
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) app_.gui.process_event(event);
  }
  return true;
}

void GameScreen::add_gamepad(SDL_JoystickID id) {
  for (SDL_Gamepad* pad : gamepads_)
    if (SDL_GetGamepadID(pad) == id) return;
  if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
    gamepads_.push_back(pad);
    const char* name = SDL_GetGamepadName(pad);
    log::info("input", std::string("controller connected: ") + (name != nullptr ? name : "(no name)"));
  } else {
    log::warning("input", std::string("a controller could not be opened: ") + SDL_GetError());
  }
}

void GameScreen::remove_gamepad(SDL_JoystickID id) {
  for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it) {
    if (SDL_GetGamepadID(*it) == id) {
      const char* name = SDL_GetGamepadName(*it);
      log::info("input", std::string("controller disconnected: ") + (name != nullptr ? name : "(no name)"));
      SDL_CloseGamepad(*it);
      gamepads_.erase(it);
      return;
    }
  }
}

void GameScreen::read_input() {
  // The script's buttons for the frame that is ending: the first is frame 1.
  std::uint32_t held = input::scripted_buttons(app_.options.script, frames_run_);
  // Keys only count while the window has the keyboard: SDL clears its key
  // state when the focus goes.
  int key_count = 0;
  const bool* keys = SDL_GetKeyboardState(&key_count);
  for (const Key& k : keys_)
    if (static_cast<int>(k.code) < key_count && keys[k.code]) held |= k.bit;
  for (SDL_Gamepad* pad : gamepads_) {
    for (const PadButton& b : pad_buttons_)
      if (SDL_GetGamepadButton(pad, b.button)) held |= b.bit;
    held |= input::directions_from_stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                                         SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= input::kTriggerThreshold)
      held |= input::bit(input::DsButton::L);
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= input::kTriggerThreshold)
      held |= input::bit(input::DsButton::R);
  }
  if (held != console_.keypad().held()) console_.keypad().set_held(held);

  // The stylus: the left button, on the touch screen, while the window has
  // the mouse. Off the screen is not touching it.
  bool touching = false;
  gfx::Point at;
  if (const input::ScriptedTouch* scripted = input::scripted_touch(app_.options.touches, frames_run_)) {
    touching = true;
    at = {scripted->x, scripted->y};
  }
  float wx = 0, wy = 0;
  const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&wx, &wy);
  if (!touching && (buttons & SDL_BUTTON_LMASK) != 0 && SDL_GetMouseFocus() == app_.window) {
    float rx = 0, ry = 0;
    if (SDL_RenderCoordinatesFromWindow(app_.renderer, wx, wy, &rx, &ry)) {
      if (const auto point = gfx::touch_point(screens_, rx, ry)) {
        touching = true;
        at = *point;
      }
    }
  }
  if (touching != touching_ || (touching && (at.x != touch_at_.x || at.y != touch_at_.y))) {
    touching_ = touching;
    touch_at_ = at;
    console_.set_touch(touching, static_cast<unsigned>(at.x), static_cast<unsigned>(at.y));
  }
}

void GameScreen::upload_screens() {
  const auto upload = [&](SDL_Texture* texture, const std::vector<std::uint32_t>& screen) {
    if (screen.size() != std::size_t{gfx::kScreenWidth} * gfx::kScreenHeight) return;
    gfx::to_rgba_bytes(screen.data(), screen.size(), pixels_.data());
    SDL_UpdateTexture(texture, nullptr, pixels_.data(), gfx::kScreenWidth * 4);
  };
  const nds::Display& display = console_.display();
  // Each screen with what the graphics hardware puts in where it is marked
  // (the frame's scene, or a capture it made), at the resolution for the
  // screens as they were last laid out.
  for (int i = 0; i < 2; ++i) {
    shown_3d_[static_cast<std::size_t>(i)] = nullptr;
    const nds::Display::ScreenFill& fill = display.frame_fill(i);
    const gfx::Rect& place = i == 0 ? screens_.top : screens_.bottom;
    if (fill.fill == nds::Display::Fill::kNone || !hardware_.is_open() || hardware_failed_ || place.h <= 0) continue;
    bool failed = false;
    SDL_Texture* t = hardware_.draw(i, i == 0 ? display.top_screen() : display.bottom_screen(),
                                    fill.fill == nds::Display::Fill::kScene ? &console_.polygon_renderer().scene() : nullptr,
                                    fill.capture, fill.brightness, scale_3d(), failed);
    if (failed) {
      give_up_hardware("draw a 3D scene");
      break;
    }
    if (t != nullptr)
      SDL_SetTextureScaleMode(t, filter_ == gfx::ScreenFilter::Sharp ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    shown_3d_[static_cast<std::size_t>(i)] = t;
  }
  if (shown_3d_[0] == nullptr) upload(top_, display.top_screen());
  if (shown_3d_[1] == nullptr) upload(bottom_, display.bottom_screen());
  enlarged_stale_ = true;
}

void GameScreen::keep_capture() {
  const nds::Display& display = console_.display();
  const std::uint64_t number = display.frame_capture();
  if (number == 0 || number == capture_kept_ || !hardware_.is_open() || hardware_failed_) return;
  capture_kept_ = number;
  if (!hardware_.capture(console_.polygon_renderer().scene(), display.capture_source(), number, scale_3d()))
    give_up_hardware("make a display capture");
}

int GameScreen::scale_3d() const {
  return ui::resolution_3d(app_.settings, static_cast<int>(std::max(screens_.top.h, screens_.bottom.h)));
}

void GameScreen::give_up_hardware(const char* why) {
  log::warning("3d", std::string("the graphics hardware failed to ") + why +
                         ", so the processor draws the 3D pictures from now on");
  hardware_failed_ = true;
  shown_3d_ = {};
  console_.display().set_hardware_3d(false);
}

void GameScreen::prepare_enlarged(int factor) {
  if (factor != enlarged_factor_) {
    for (SDL_Texture*& t : enlarged_) {
      if (t != nullptr) SDL_DestroyTexture(t);
      t = SDL_CreateTexture(app_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                            gfx::kScreenWidth * factor, gfx::kScreenHeight * factor);
      if (t == nullptr) {
        log::warning("window", std::string("cannot make the enlarged screens, so they are shown sharp: ") +
                                   SDL_GetError());
        enlarging_failed_ = true;
        return;
      }
      SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
      SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    }
    enlarged_factor_ = factor;
    enlarged_stale_ = true;
    log::writef(log::Level::Debug, "window", "screens enlarged %d times before smoothing", factor);
  }
  if (!enlarged_stale_) return;
  SDL_Texture* const sources[2] = {top_, bottom_};
  for (int i = 0; i < 2; ++i) {
    SDL_SetRenderTarget(app_.renderer, enlarged_[static_cast<std::size_t>(i)]);
    SDL_RenderTexture(app_.renderer, sources[i], nullptr, nullptr);
  }
  SDL_SetRenderTarget(app_.renderer, nullptr);
  enlarged_stale_ = false;
}

void GameScreen::draw() {
  int width = 0, height = 0;
  SDL_GetCurrentRenderOutputSize(app_.renderer, &width, &height);
  screens_ = gfx::lay_out(width, height, layout_);
  SDL_Texture* top = top_;
  SDL_Texture* bottom = bottom_;
  if (filter_ == gfx::ScreenFilter::SharpSmoothEdges && !enlarging_failed_) {
    prepare_enlarged(gfx::prescale_factor(screens_.scale, largest_texture_));
    if (!enlarging_failed_) {
      top = enlarged_[0];
      bottom = enlarged_[1];
    }
  }
  SDL_SetRenderDrawColor(app_.renderer, 0, 0, 0, 255);
  SDL_RenderClear(app_.renderer);
  const auto place = [&](SDL_Texture* texture, const gfx::Rect& r) {
    if (r.w <= 0 || r.h <= 0) return;  // Not shown (the touch screen alone).
    const SDL_FRect to{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                       static_cast<float>(r.h)};
    SDL_RenderTexture(app_.renderer, texture, nullptr, &to);
  };
  // The graphics hardware's picture is already enlarged a whole number of
  // times, so it is only smoothed (unless sharp) the rest of the way.
  if (shown_3d_[0] != nullptr) top = shown_3d_[0];
  if (shown_3d_[1] != nullptr) bottom = shown_3d_[1];
  place(top, screens_.top);
  place(bottom, screens_.bottom);
}

void GameScreen::capture(bool last) {
  SDL_Surface* surface = SDL_RenderReadPixels(app_.renderer, nullptr);
  if (surface == nullptr) {
    log::error("window", std::string("cannot read the window back: ") + SDL_GetError());
    std::fprintf(stderr, "error: cannot read the window back: %s\n", SDL_GetError());
    return;
  }
  std::filesystem::path file = app_.options.capture;
  if (!last) file.replace_filename(file.stem().string() + "-" + std::to_string(frames_run_) + file.extension().string());
  const std::string name = file.string();
  if (SDL_SaveBMP(surface, name.c_str())) {
    if (last) {
      captured_ = true;
      log::info("window", "the last frame was written to " + name);
      std::printf("The last frame was written to %s\n", name.c_str());
    }
  } else {
    log::error("window", "cannot write " + name + ": " + SDL_GetError());
    std::fprintf(stderr, "error: cannot write %s: %s\n", name.c_str(), SDL_GetError());
  }
  SDL_DestroySurface(surface);
}

// The window's title shows how many of the game's frames are run a second
// (59.83 on a console; fewer on a computer too slow for it) and how many
// pictures are shown (presented). Called once for every picture and once for
// every frame run unshown; `new_frame` says whether this is the first call
// for a frame, `shown` whether a picture was presented.
// The log has the same once a minute, and every second in detail.
void GameScreen::count_rates(bool new_frame, bool shown) {
  if (new_frame) ++frames_this_second_;
  if (shown) ++pictures_this_second_;
  const std::uint64_t now = SDL_GetTicksNS();
  if (now - second_started_ < 1'000'000'000ull) return;
  const double seconds = static_cast<double>(now - second_started_) / 1e9;
  const double frames = static_cast<double>(frames_this_second_) / seconds;
  const double pictures = static_cast<double>(pictures_this_second_) / seconds;
  char text[64];
  std::snprintf(text, sizeof text, " - game %.1f frames/s, %.1f pictures/s", frames, pictures);
  SDL_SetWindowTitle(app_.window, (title_ + text).c_str());
  ++rates_logged_;
  const log::Level level = rates_logged_ % 60 == 0 ? log::Level::Info : log::Level::Debug;
  log::writef(level, "game", "frame %" PRIu64 ": %.1f frames/s, %.1f pictures/s", frames_run_, frames, pictures);
  second_started_ = now;
  frames_this_second_ = pictures_this_second_ = 0;
}

bool GameScreen::run_menu() {
  log::info("game", "paused: the menu is open");
  menu_page_ = MenuPage::Main;
  // Nothing held while the menu is open.
  console_.keypad().set_held(0);
  if (touching_) {
    touching_ = false;
    console_.set_touch(false, 0, 0);
  }
  while (menu_open_) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      app_.gui.process_event(event);
      if (event.type == SDL_EVENT_QUIT) {
        log::info("window", "closed");
        end_ = GameEnd::Quit;
        return false;
      }
      if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !book_.waiting_for_input()) {
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
          // Escape goes back a page, and from the first closes the menu.
          if (menu_page_ == MenuPage::Settings) menu_page_ = MenuPage::Main;
          else menu_open_ = false;
        } else if (!ImGui::GetIO().WantTextInput) {
          handle_window_key(event.key);
        }
      }
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE &&
          !book_.waiting_for_input())
        menu_open_ = false;
      if (event.type == SDL_EVENT_RENDER_TARGETS_RESET || event.type == SDL_EVENT_RENDER_DEVICE_RESET)
        enlarged_stale_ = true;
      if (event.type == SDL_EVENT_GAMEPAD_ADDED) add_gamepad(event.gdevice.which);
      if (event.type == SDL_EVENT_GAMEPAD_REMOVED) remove_gamepad(event.gdevice.which);
    }
    if (app_.settings.revision() != settings_seen_) apply_settings(false);
    draw();
    app_.gui.begin_frame();
    draw_menu();
    app_.gui.end_frame();
    SDL_RenderPresent(app_.renderer);
    if (!vsync_) SDL_Delay(15);
    if (end_ != GameEnd::Running) return false;
  }
  log::info("game", "continued");
  clock_.restart(SDL_GetTicksNS());
  second_started_ = SDL_GetTicksNS();
  frames_this_second_ = pictures_this_second_ = 0;
  return true;
}

void GameScreen::draw_menu() {
  const ImGuiViewport* view = ImGui::GetMainViewport();
  // The game is dimmed behind the menu.
  ImGui::GetBackgroundDrawList()->AddRectFilled(view->Pos, ImVec2(view->Pos.x + view->Size.x, view->Pos.y + view->Size.y),
                                                IM_COL32(0, 0, 0, 150));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings;
  if (menu_page_ == MenuPage::Main) {
    const float width = ImGui::GetFontSize() * 18;
    ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x / 2, view->Pos.y + view->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::Begin("Paused", nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize);
    const ImVec2 button(-1, 0);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button("Continue", button)) menu_open_ = false;
    if (ImGui::Button("Settings", button)) menu_page_ = MenuPage::Settings;
    if (ImGui::Button("Report a problem", button)) report_problem(app_);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Opens the form for a problem report on GitHub, and the log folder.\n"
                        "Attach the log; never the ROM, saves or other game files.");
    if (can_go_back_ && ImGui::Button("Back to the launcher", button)) {
      log::info("game", "back to the launcher");
      end_ = GameEnd::ToLauncher;
    }
    if (ImGui::Button("Quit", button)) {
      log::info("game", "quit from the menu");
      end_ = GameEnd::Quit;
    }
    ImGui::End();
    return;
  }
  ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x * 0.05f, view->Pos.y + view->Size.y * 0.05f));
  ImGui::SetNextWindowSize(ImVec2(view->Size.x * 0.9f, view->Size.y * 0.9f));
  ImGui::Begin("Settings", nullptr, flags);
  if (ImGui::Button("Back")) menu_page_ = MenuPage::Main;
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Changes take effect at once and are kept.");
  ImGui::PopStyleColor();
  app_.settings_changed(book_.draw(app_.settings));
  ImGui::End();
}

}  // namespace bd::app
