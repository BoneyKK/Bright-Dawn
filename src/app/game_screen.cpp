#include "app/game_screen.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/version.h"
#include "debug/frame_log.h"
#include "gfx/screen_image.h"
#include "imgui.h"
#include "nds/scene_dump.h"
#include "ui/game_settings.h"

namespace bd::app {

namespace key = ui::key;

static_assert(input::bit(input::DsButton::A) == game::OpeningSkip::kA &&
              input::bit(input::DsButton::B) == game::OpeningSkip::kB &&
              input::bit(input::DsButton::Start) == game::OpeningSkip::kStart);

namespace {

// The console's main memory, for the skip of the opening.
class ConsoleMemory : public game::GuestMemory {
 public:
  explicit ConsoleMemory(nds::Console& console) : console_(console) {}
  std::uint32_t read32(std::uint32_t address) const override { return console_.bus().main_read(address, 4); }
  void write32(std::uint32_t address, std::uint32_t value) override {
    console_.bus().main_write(address, 4, value);
    console_.machine().note_memory_written(address, 4);
  }

 private:
  nds::Console& console_;
};

}  // namespace

GameScreen::GameScreen(App& app, nds::Console& console, bool can_go_back)
    : app_(app), console_(&console), can_go_back_(can_go_back) {}

GameScreen::~GameScreen() {
  if (app_.options.check_2d)
    log::writef(log::Level::Info, "2d", "check: %" PRIu64 " of %" PRIu64 " 2D pictures drawn by the graphics hardware differ",
                differed_2d_, checked_2d_);
  frame_log::set_enabled(false);
  frame_log::state().sync = false;
  detach();
  for (SDL_Gamepad* pad : gamepads_) SDL_CloseGamepad(pad);
  if (top_ != nullptr) SDL_DestroyTexture(top_);
  if (bottom_ != nullptr) SDL_DestroyTexture(bottom_);
  for (SDL_Texture* t : enlarged_)
    if (t != nullptr) SDL_DestroyTexture(t);
}

void GameScreen::detach() {
  if (console_ != nullptr) console_->sound_hardware().on_output = nullptr;
  console_ = nullptr;
}

void GameScreen::attach(nds::Console& console) {
  detach();
  console_ = &console;
  console_->sound_hardware().on_output = [this](nds::SoundHardware::Output o) { sound_.add(o.left, o.right); };
  // What was kept of the console before: its captures are numbered anew.
  capture_kept_ = 0;
  shown_3d_ = {};
  hardware_.forget_captures();
  apply_settings(false);
  log_executed_ = console_->machine().executed();
  log_interpreted_ = console_->machine().statistics().interpreted_instructions;
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
  console_->sound_hardware().on_output = [this](nds::SoundHardware::Output o) { sound_.add(o.left, o.right); };
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
  // The frame log (launcher, Debug page), when it is on: each frame from
  // here on is a row.
  if (app_.settings.toggle(ui::key::kFrameLog)) {
    const std::filesystem::path path = frame_log_path();
    frame_log_ = std::make_unique<frame_log::File>(path);
    if (frame_log_->is_open()) {
      frame_log::state().sync = app_.settings.toggle(ui::key::kFrameLogSync);
      frame_log::set_enabled(true);
      log::info("frames", "the frame log is written to " + utf8(path) +
                              (frame_log::state().sync ? " (sync mode)" : " (normal mode)"));
    } else {
      log::warning("frames", "the frame log cannot be written to " + utf8(path));
      frame_log_.reset();
    }
  }
  apply_settings(true);
  if (frame_log_) {
    // From here the console runs the game's code until the first frame ends.
    frame_log::restart(frame_log::kArm9);
    log_executed_ = console_->machine().executed();
    log_interpreted_ = console_->machine().statistics().interpreted_instructions;
  }
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
  enlarged_stale_ = {true, true};
  // The 3D scenes: by the graphics hardware where it can, else here.
  bool hardware = ui::hardware_3d(s);
  if (hardware && !hardware_tried_) {
    hardware_tried_ = true;
    std::string why;
    if (!hardware_.open(app_.renderer, why)) {
      log::warning("3d", "the graphics hardware cannot draw 3D scenes, so the processor draws them: " + why);
      hardware_problem_ = "the graphics hardware cannot draw 3D scenes: " + why;
    }
  }
  const bool wanted = hardware;
  hardware = hardware && hardware_.is_open() && !hardware_failed_;
  if (wanted && !hardware) console_->display().hardware_3d_failed(hardware_problem_);
  else console_->display().set_hardware_3d(hardware);
  // The 2D pictures: by the graphics hardware too, unless asked otherwise.
  console_->display().set_hardware_2d(hardware && !s.toggle(key::kProcessor2D));
  console_->display().set_check_2d(app_.options.check_2d);
  audio_.set(s.toggle(key::kSound), static_cast<int>(s.number(key::kVolume)));
  if (!hardware) shown_3d_ = {};
  console_->polygon_renderer().set_edge_marking(ui::edge_marking(s));
  console_->polygon_renderer().keep_inputs(!app_.options.dump_3d.empty());
  const bool smooth = ui::smooth_textures(s);
  console_->polygon_renderer().set_texture_filter(smooth ? nds::PolygonRenderer::TextureFilter::Bilinear
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
  // The scene is made during the frame, for the resolution it is drawn at.
  console_->polygon_renderer().set_fine_positions(app_.settings.toggle(key::kFineCoordinates) && scale_3d() > 1);
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
  if (!app_.options.dump_3d.empty() && frames_run_ == app_.options.frames) dump_3d();
  if (!show && !capture_now) {
    count_rates(true, false);
    return true;
  }
  upload_screens();
  for (bool first = true;; first = false) {
    // With the frame log on, the filter passes are recorded and the wait
    // for the swapchain is made before the picture is presented, so that
    // each has a time of its own.
    {
      frame_log::Scope frame_log_scope(frame_log::kFilter);
      draw();
      if (frame_log::enabled()) SDL_FlushRenderer(app_.renderer);
    }
    if (first && capture_now) capture(capture_last);
    if (frame_log::enabled() && app_.gpu != nullptr) {
      frame_log::Scope frame_log_scope(frame_log::kVsyncWait);
      SDL_WaitForGPUSwapchain(app_.gpu, app_.window);
    }
    {
      frame_log::Scope frame_log_scope(frame_log::kPresent);
      SDL_RenderPresent(app_.renderer);
    }
    if (frame_log::state().sync && app_.gpu != nullptr) {
      frame_log::Scope frame_log_scope(frame_log::kPresentGpu);
      SDL_WaitForGPUIdle(app_.gpu);
    }
    ++frame_log::state().counts.pictures;
    count_rates(first, true);
    if (app_.options.unthrottled) break;
    if (!vsync_) {
      const std::uint64_t wait = clock_.until_due();
      frame_log::Scope frame_log_scope(frame_log::kPaceWait);
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

// The frame log's row: the times since the last row (they add
// up to total_us), then counts, then flags.
void GameScreen::log_frame() {
  if (!frame_log_) return;
  frame_log::State& s = frame_log::state();
  frame_log::switch_to(s.current);  // The times up to now.
  const double per_us = frame_log_->ticks_per_us();
  const nds::Display& display = console_->display();
  const bool hardware = hardware_.is_open() && !hardware_failed_ && ui::hardware_3d(app_.settings);
  if (!frame_log_->started()) {
    std::vector<std::string> names{"frame", "total_us"};
    for (const char* n : frame_log::kColumnNames) names.emplace_back(n);
    for (const char* n : {"arm9_instructions", "interpreted_instructions", "polygons", "vertices", "gpu_vertices", "scene_draws",
                          "scene_textures", "textures_decoded", "decoded_bytes", "textures_uploaded", "texture_bytes",
                          "screen_bytes", "video_bytes", "vertex_bytes", "bank_bytes", "raster_bytes", "gpu_captures", "pictures",
                          "timer_switches", "shown", "gpu_3d", "3d_top", "3d_bottom", "capture_made", "capture_shown",
                          "raster_ran", "edge_marking", "anti_aliasing", "fog", "scale", "main_on_top"})
      names.emplace_back(n);
    std::string driver = "none", device = "none", version;
    if (app_.gpu != nullptr) {
      if (const char* d = SDL_GetGPUDeviceDriver(app_.gpu)) driver = d;
      const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(app_.gpu);
      if (const char* n = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr)) device = n;
      const char* dn = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr);
      const char* dv = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, nullptr);
      const char* di = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, nullptr);
      version = std::string(dn != nullptr ? dn : "") + (dv != nullptr ? std::string(" ") + dv : "") +
                (di != nullptr ? std::string(" (") + di + ")" : "");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(app_.window, &w, &h);
    const int scale = scale_3d();
    const char* filter = filter_ == gfx::ScreenFilter::SharpSmoothEdges ? "sharp pixels, smooth edges"
                         : filter_ == gfx::ScreenFilter::Smooth         ? "smooth"
                                                                        : "sharp";
    char line[1024];
    std::snprintf(line, sizeof line,
                  "Bright Dawn frame log | GPU driver: %s, %s | device: %s | window: %d x %d | 3D resolution: %dx "
                  "(%d x %d, setting %s) | screen filter: %s | renderer: %s, 3D by %s | mode: %s",
                  driver.c_str(), version.empty() ? "version unknown" : version.c_str(), device.c_str(), w, h, scale,
                  256 * scale, 192 * scale, app_.settings.choice(ui::key::k3DResolution).c_str(), filter,
                  SDL_GetRendererName(app_.renderer) != nullptr ? SDL_GetRendererName(app_.renderer) : "none",
                  hardware ? "the graphics hardware" : "the processor",
                  s.sync ? "sync (each GPU pass is waited for: its time is its GPU time; the frame rate is lower)"
                         : "normal (CPU times only: GPU passes are not waited for)");
    char second[512];
    std::snprintf(second, sizeof second,
                  "times in microseconds, each frame split so that the times add up to total_us; the timer costs "
                  "about %.0f ns a switch (timer_switches); vsync %s; game code: %s",
                  frame_log_->ns_per_switch(), vsync_ ? "on" : "off", title_.c_str());
    frame_log_->start({line, second}, names);
  }
  std::vector<double> v;
  v.reserve(80);
  v.push_back(static_cast<double>(frames_run_));
  double total = 0;
  for (std::uint64_t t : s.spent) total += static_cast<double>(t);
  v.push_back(total / per_us);
  for (std::uint64_t t : s.spent) v.push_back(static_cast<double>(t) / per_us);
  const std::uint64_t executed = console_->machine().executed();
  const std::uint64_t interpreted = console_->machine().statistics().interpreted_instructions;
  const std::uint64_t all = executed - log_executed_, by_interpreter = interpreted - log_interpreted_;
  log_executed_ = executed;
  log_interpreted_ = interpreted;
  const frame_log::Counts& c = s.counts;
  for (std::uint64_t n : {all - std::min(all, by_interpreter), by_interpreter, c.polygons, c.vertices, c.gpu_vertices, c.scene_draws,
                          c.scene_textures, c.textures_decoded, c.decoded_bytes, c.textures_uploaded, c.texture_bytes,
                          c.screen_bytes, c.video_bytes, c.vertex_bytes, c.bank_bytes, c.raster_bytes, c.gpu_captures, c.pictures,
                          s.switches})
    v.push_back(static_cast<double>(n));
  nds::DisplayEngine& main = console_->display().main_engine();
  const bool main_on_top = (display.power_control() & nds::Display::kMainOnTop) != 0;
  const std::uint32_t control = main.control();
  const bool main_3d = (control & 0x108u) == 0x108u && ((control >> 16) & 3) == 1;
  const auto filled = [&](int i) { return display.frame_fill(i).fill != nds::Display::Fill::kNone; };
  const auto captured = [&](int i) { return display.frame_fill(i).fill == nds::Display::Fill::kCapture; };
  const bool top_3d = filled(0) || (main_on_top && main_3d), bottom_3d = filled(1) || (!main_on_top && main_3d);
  for (bool f : {c.pictures != 0, hardware, top_3d, bottom_3d, c.capture_made, captured(0) || captured(1), c.raster_ran,
                 c.edge_marking, c.anti_aliasing, c.fog})
    v.push_back(f ? 1 : 0);
  v.push_back(hardware ? scale_3d() : 1);
  v.push_back(main_on_top ? 1 : 0);
  frame_log_->row(v);
  // The next row: times from now; the scene's flags stay until the next scene.
  for (std::uint64_t& t : s.spent) t = 0;
  s.switches = 0;
  const frame_log::Counts kept = s.counts;
  s.counts = frame_log::Counts{};
  s.counts.edge_marking = kept.edge_marking;
  s.counts.anti_aliasing = kept.anti_aliasing;
  s.counts.fog = kept.fog;
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
        enlarged_stale_ = {true, true};
        break;
      case SDL_EVENT_RENDER_DEVICE_RESET:
        log::warning("window", "the graphics device was reset");
        enlarged_stale_ = {true, true};
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
  ConsoleMemory memory(*console_);
  held = opening_skip_.frame(memory, held, app_.settings.toggle(key::kSkipOpening));
  if (opening_skip_.skipping() && !was_skipping_) log::info("game", "the opening is skipped (Start), up to the title");
  was_skipping_ = opening_skip_.skipping();
  if (held != console_->keypad().held()) console_->keypad().set_held(held);

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
    console_->set_touch(touching, static_cast<unsigned>(at.x), static_cast<unsigned>(at.y));
  }
}

void GameScreen::upload_screens() {
  const auto upload = [&](SDL_Texture* texture, const std::vector<std::uint32_t>& screen) {
    if (screen.size() != std::size_t{gfx::kScreenWidth} * gfx::kScreenHeight) return;
    frame_log::Scope frame_log_scope(frame_log::kUpload);
    frame_log::state().counts.screen_bytes += screen.size() * 4;
    gfx::to_rgba_bytes(screen.data(), screen.size(), pixels_.data());
    SDL_UpdateTexture(texture, nullptr, pixels_.data(), gfx::kScreenWidth * 4);
  };
  const nds::Display& display = console_->display();
  // Each screen with what the graphics hardware puts in where it is marked
  // (the frame's scene, or a capture it made), at the resolution for the
  // screens as they were last laid out.
  // A screen whose 2D picture the processor did not draw is drawn by the
  // graphics hardware from its lines (nds/frame_2d.h), with nothing put in
  // where nothing is.
  for (int i = 0; i < 2; ++i) {
    shown_3d_[static_cast<std::size_t>(i)] = nullptr;
    const nds::Display::ScreenFill& fill = display.frame_fill(i);
    const nds::Frame2D& lines = display.screen_2d(i);
    const gfx::Rect& place = i == 0 ? screens_.top : screens_.bottom;
    if ((fill.fill == nds::Display::Fill::kNone && !lines.by_hardware) || !hardware_.is_open() || hardware_failed_ ||
        place.h <= 0)
      continue;
    Hardware3D::Picture picture;
    if (lines.by_hardware) {
      picture.lines = &lines;
      picture.video = &display.video_2d();
    } else {
      picture.pixels = i == 0 ? &display.top_screen() : &display.bottom_screen();
    }
    bool failed = false;
    SDL_Texture* t = hardware_.draw(i, picture,
                                    fill.fill == nds::Display::Fill::kScene ? &console_->polygon_renderer().scene() : nullptr,
                                    fill.capture, fill.brightness, scale_3d(), failed);
    if (failed) {
      give_up_hardware(lines.by_hardware ? "draw a 2D picture" : "draw a 3D scene");
      break;
    }
    if (lines.by_hardware && app_.options.check_2d) check_2d(i, fill.fill != nds::Display::Fill::kNone);
    if (t != nullptr)
      SDL_SetTextureScaleMode(t, filter_ == gfx::ScreenFilter::Sharp ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    shown_3d_[static_cast<std::size_t>(i)] = t;
  }
  if (shown_3d_[0] == nullptr) upload(top_, display.top_screen());
  if (shown_3d_[1] == nullptr) upload(bottom_, display.bottom_screen());
  enlarged_stale_ = {true, true};
}

void GameScreen::check_2d(int i, bool with_marks) {
  const nds::Display& display = console_->display();
  std::vector<std::uint32_t> pixels;
  if (!hardware_.read_2d(pixels)) return;
  const std::vector<std::uint32_t>& drawn = i == 0 ? display.top_screen() : display.bottom_screen();
  const auto eight = [](std::uint32_t six) { return (six & 63) << 2 | (six & 63) >> 4; };
  std::size_t differ = 0, first = 0;
  for (std::size_t n = 0; n < pixels.size() && n < drawn.size(); ++n) {
    const std::uint32_t c = drawn[n];
    const std::uint32_t wanted = eight(c) | eight(c >> 6) << 8 | eight(c >> 12) << 16 | (with_marks ? c >> 24 << 24 : 0);
    if (pixels[n] == wanted) continue;
    if (differ++ == 0) first = n;
  }
  ++checked_2d_;
  if (differ == 0) return;
  if (++differed_2d_ > 20) return;
  const std::uint32_t* w = &display.screen_2d(i).lines[first / 256 * nds::Frame2D::kWords];
  const std::uint32_t c = drawn[first];
  const std::uint32_t wanted = eight(c) | eight(c >> 6) << 8 | eight(c >> 12) << 16 | (with_marks ? c >> 24 << 24 : 0);
  log::writef(log::Level::Warning, "2d",
              "check: frame %" PRIu64 ", %s screen: %zu pixels differ; the first at (%zu, %zu): %08x, not %08x; "
              "control %08x, backgrounds %08x %08x, blend %08x, brightness %08x",
              frames_run_, i == 0 ? "top" : "bottom", differ, first % 256, first / 256, pixels[first], wanted, w[0],
              w[1], w[2], w[19], w[20]);
}

void GameScreen::keep_capture() {
  const nds::Display& display = console_->display();
  const std::uint64_t number = display.frame_capture();
  if (number == 0 || number == capture_kept_ || !hardware_.is_open() || hardware_failed_) return;
  capture_kept_ = number;
  if (!hardware_.capture(console_->polygon_renderer().scene(), display.capture_recipe(), display.capture_source(), number,
                         scale_3d()))
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
  hardware_problem_ = std::string("the graphics hardware failed to ") + why;
  console_->display().hardware_3d_failed(hardware_problem_);
}

SDL_Texture* GameScreen::enlarged(int i, SDL_Texture* source, int factor) {
  const std::size_t n = static_cast<std::size_t>(i);
  SDL_Texture*& t = enlarged_[n];
  if (factor != enlarged_factor_[n]) {
    if (t != nullptr) SDL_DestroyTexture(t);
    t = SDL_CreateTexture(app_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, gfx::kScreenWidth * factor,
                          gfx::kScreenHeight * factor);
    if (t == nullptr) {
      log::warning("window", std::string("cannot make the enlarged screens, so they are shown sharp: ") +
                                 SDL_GetError());
      enlarging_failed_ = true;
      return nullptr;
    }
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    enlarged_factor_[n] = factor;
    enlarged_stale_[n] = true;
    log::writef(log::Level::Debug, "window", "screen %d enlarged %d times before smoothing", i, factor);
  }
  if (enlarged_stale_[n]) {
    SDL_SetTextureScaleMode(source, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderTarget(app_.renderer, t);
    SDL_RenderTexture(app_.renderer, source, nullptr, nullptr);
    SDL_SetRenderTarget(app_.renderer, nullptr);
    enlarged_stale_[n] = false;
  }
  return t;
}

void GameScreen::draw() {
  int width = 0, height = 0;
  SDL_GetCurrentRenderOutputSize(app_.renderer, &width, &height);
  screens_ = gfx::lay_out(width, height, layout_);
  // Each screen: the graphics hardware's picture where it made one (at a
  // whole multiple of the DS screen's size), else the console's own. For
  // SharpSmoothEdges it is first enlarged without smoothing to the whole
  // multiple of itself at or above the window's scale, then smoothed to
  // the window, so that each of its pixels is a sharp block and only the
  // window pixel where two meet is a mix of them.
  std::array<SDL_Texture*, 2> shown{top_, bottom_};
  for (std::size_t i = 0; i < 2; ++i) {
    if (shown_3d_[i] != nullptr) shown[i] = shown_3d_[i];
    if (filter_ != gfx::ScreenFilter::SharpSmoothEdges || enlarging_failed_) continue;
    float w = 0, h = 0;
    SDL_GetTextureSize(shown[i], &w, &h);
    const int made_at = std::max(1, static_cast<int>(w) / gfx::kScreenWidth);
    const int factor = gfx::enlarge_factor(filter_, screens_.scale, largest_texture_, made_at);
    if (factor == made_at) continue;  // Already a whole multiple at or above the scale.
    if (SDL_Texture* e = enlarged(static_cast<int>(i), shown[i], factor); e != nullptr) shown[i] = e;
  }
  SDL_SetRenderDrawColor(app_.renderer, 0, 0, 0, 255);
  SDL_RenderClear(app_.renderer);
  const auto place = [&](SDL_Texture* texture, const gfx::Rect& r) {
    if (r.w <= 0 || r.h <= 0) return;  // Not shown (the touch screen alone).
    const SDL_FRect to{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                       static_cast<float>(r.h)};
    SDL_RenderTexture(app_.renderer, texture, nullptr, &to);
  };
  place(shown[0], screens_.top);
  place(shown[1], screens_.bottom);
}

void GameScreen::dump_3d() {
  const nds::Display& display = console_->display();
  nds::SceneDump dump;
  dump.inputs = console_->polygon_renderer().kept_inputs();
  dump.edge_marking = console_->polygon_renderer().edge_marking();
  dump.fine_positions = console_->polygon_renderer().fine_positions();
  dump.scale = scale_3d();
  for (int i = 0; i < 2; ++i) {
    const nds::Display::ScreenFill& fill = display.frame_fill(i);
    const auto n = static_cast<std::size_t>(i);
    dump.screens[n] = i == 0 ? display.top_screen() : display.bottom_screen();
    dump.fills[n] = fill.fill == nds::Display::Fill::kScene ? 1 : fill.fill == nds::Display::Fill::kNone ? 0 : 2;
    dump.brightness[n] = fill.brightness;
  }
  std::string error;
  const std::string name = app_.options.dump_3d.string();
  if (dump.inputs.frame.polygons.empty()) {
    std::fprintf(stderr, "error: no 3D scene to write to %s\n", name.c_str());
  } else if (nds::write_scene_dump(app_.options.dump_3d, dump, error)) {
    log::info("3d", "the 3D scene of the last frame was written to " + name);
    std::printf("The 3D scene of the last frame was written to %s\n", name.c_str());
  } else {
    log::error("3d", error);
    std::fprintf(stderr, "error: %s\n", error.c_str());
  }
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
  console_->keypad().set_held(0);
  if (touching_) {
    touching_ = false;
    console_->set_touch(false, 0, 0);
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
        enlarged_stale_ = {true, true};
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
