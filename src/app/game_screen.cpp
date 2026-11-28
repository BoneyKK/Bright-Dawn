#include "app/game_screen.h"
#include "app/frame_overlay.h"
#include "app/system_info.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

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

// The console's main memory, for the skip of the opening and the stick's
// walk.
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
  write_perf_report();
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
  if (console_ != nullptr) {
    console_->sound_hardware().on_output = nullptr;
    console_->sound_driver().sequences().on_prepare = nullptr;
  }
  if (music_) music_->stop_all();
  console_ = nullptr;
}

void GameScreen::connect_sound() {
  console_->sound_hardware().on_output = [this](nds::SoundHardware::Output o) {
    // As Sound quality and Interpolation say (nds/sound_hardware.h).
    mixer_.add_console(o.fine_left, o.fine_right);
  };
  console_->sound_driver().sequences().on_prepare = [this](unsigned player, std::uint32_t starts, std::uint32_t data) {
    if (!music_ || !nds::Arm9Bus::in_main_memory(data)) return false;
    const ByteSpan memory(console_->bus().main_memory(), nds::Arm9Bus::kMainRamSize);
    return music_->prepared(player, starts, memory, data & (nds::Arm9Bus::kMainRamSize - 1));
  };
}

void GameScreen::set_music(std::unique_ptr<audio::MusicReplacer> music) { music_ = std::move(music); }

void GameScreen::update_music() {
  if (!music_ || console_ == nullptr) return;
  nds::SequencePlayer& sequences = console_->sound_driver().sequences();
  std::array<audio::PlayerState, audio::MusicReplacer::kPlayers> players{};
  for (unsigned p = 0; p < audio::MusicReplacer::kPlayers; ++p) {
    const nds::SequencePlayer::PlayerInfo info = sequences.player_info(p);
    players[p] = audio::PlayerState{info.prepared, info.playing, info.paused, info.starts, info.ends, info.decibels};
  }
  const auto master = static_cast<float>(console_->sound_hardware().read(nds::SoundHardware::kControl, 2) & 0x7F) / 127.0f;
  music_->update(players, master, mixer_);
  for (std::uint32_t failed = music_->take_failed(); failed != 0; failed &= failed - 1)
    sequences.set_silent(static_cast<unsigned>(std::countr_zero(failed)), false);
}

void GameScreen::attach(nds::Console& console) {
  detach();
  console_ = &console;
  connect_sound();
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
  connect_sound();
  if (!app_.options.record_sound.empty()) {
    std::string why;
    if (mixer_.record(app_.options.record_sound, why))
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
  // The performance report (--perf-report FILE; launcher, Debug page).
  if (!app_.options.perf_report.empty()) perf_path_ = app_.options.perf_report;
  else if (app_.settings.toggle(ui::key::kPerfReport)) perf_path_ = perf_report_path();
  if (!perf_path_.empty()) {
    perf_ = std::make_unique<debug::PerfReport>();
    for (const std::string& note : app_.start_notes) perf_->note(0, "fallback", note);
    if (!frame_log_) frame_log::state().sync = app_.settings.toggle(ui::key::kFrameLogSync);
    log::info("frames", "the performance report is written at the end, to " + utf8(perf_path_));
  }
  // Frame by frame: --pause-at and --step-count.
  stepper_.script(app_.options.pause_at, app_.options.step_count);
  describe_gpu();
  apply_settings(true);
  if (frame_log::enabled()) {
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
  // The debug hotkeys (launcher, Debug page), each while its setting is on.
  pause_step_ = s.toggle(key::kPauseStep) || app_.options.pause_at != 0;
  overlay_allowed_ = s.toggle(key::kFrameOverlay);
  if (first || !overlay_allowed_) overlay_shown_ = overlay_allowed_;
  capture_hotkey_ = s.toggle(key::kCaptureHotkey);
  debug_keys_.clear();
  for (const ui::Hotkey h : {ui::Hotkey::PauseFrame, ui::Hotkey::StepFrame, ui::Hotkey::ResumeFrame, ui::Hotkey::FrameOverlay,
                             ui::Hotkey::CaptureFrame}) {
    const bool on = h == ui::Hotkey::FrameOverlay ? overlay_allowed_ : h == ui::Hotkey::CaptureFrame ? capture_hotkey_ : pause_step_;
    if (!on) continue;
    const std::string k = ui::hotkey_binding(s, h, true), b = ui::hotkey_binding(s, h, false);
    debug_keys_.push_back({h, k.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(k.c_str()),
                           b.empty() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(b.c_str())});
  }
  // The overlay and the capture hotkey use the frame log's rows: it runs
  // without its file.
  if ((overlay_allowed_ || capture_hotkey_ || perf_) && !frame_log::enabled()) {
    ticks_per_us_ = frame_log::measure_ticks_per_us();
    frame_log::set_enabled(true);
    frame_log::restart(frame_log::kArm9);
  }
  // The known GPU issues' workarounds (debug/gpu_info.h), unless switched
  // off on the Debug page.
  workarounds_ = 0;
  if (s.toggle(key::kGpuWorkarounds))
    for (const debug::GpuIssue* issue : gpu_issues_) workarounds_ |= issue->workarounds;
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
  bool hardware = ui::hardware_3d(s) && (workarounds_ & debug::kWorkaroundProcessor3D) == 0;
  if (hardware && !hardware_tried_) {
    hardware_tried_ = true;
    std::string why;
    if (!hardware_.open(app_.renderer, why)) {
      log::warning("3d", "the graphics hardware cannot draw 3D scenes, so the processor draws them: " + why);
      hardware_problem_ = "the graphics hardware cannot draw 3D scenes: " + why;
      if (perf_) perf_->note(frames_run_, "gpu_error", hardware_problem_);
    }
  }
  const bool wanted = hardware;
  hardware = hardware && hardware_.is_open() && !hardware_failed_;
  if (wanted && !hardware) console_->display().hardware_3d_failed(hardware_problem_);
  else console_->display().set_hardware_3d(hardware);
  // The 2D pictures: by the graphics hardware too, unless asked otherwise.
  console_->display().set_hardware_2d(hardware && !s.toggle(key::kProcessor2D) &&
                                      (workarounds_ & debug::kWorkaroundProcessor2D) == 0);
  console_->display().set_check_2d(app_.options.check_2d);
  audio_.set(s.toggle(key::kSound), static_cast<int>(s.number(key::kVolume)));
  console_->sound_hardware().set_full_precision(s.value(key::kSoundQuality) != "original");
  console_->sound_hardware().set_interpolation(
      nds::interpolation_named(s.value(key::kInterpolation)).value_or(nds::Interpolation::kNone));
  const bool replace = s.toggle(key::kReplaceMusic);
  if (replace_music_ && !replace && music_) {
    // The game's own music again, from now on.
    music_->stop_all();
    for (unsigned p = 0; p < nds::SequencePlayer::kPlayers; ++p) console_->sound_driver().sequences().set_silent(p, false);
  }
  replace_music_ = replace;
  if (music_) {
    music_->set_enabled(replace);
    music_->set_volume(static_cast<float>(s.number(key::kMusicVolume)) / 100.0f);
  }
  if (!hardware) shown_3d_ = {};
  // Widescreen: the main engine's screen a third wider, where the graphics
  // hardware draws.
  const bool was_wide = wide_;
  wide_ = hardware && s.toggle(key::kWidescreen);
  hardware_.set_wide(wide_);
  console_->geometry().set_wide(wide_);
  console_->display().set_sides(wide_);
  layout_.wide_screen = wide_ ? main_screen() : -1;
  // The widescreen HUD: at the wide screen's edges, or centred as the game
  // puts it (nds::WideHud).
  wide_hud_ = wide_ && s.choice(key::kWidescreenHud) == "wide";
  console_->geometry().set_wide_hud(wide_hud_);
  console_->polygon_renderer().set_edge_marking(ui::edge_marking(s));
  console_->polygon_renderer().keep_inputs(!app_.options.dump_3d.empty());
  const bool smooth = ui::smooth_textures(s);
  console_->polygon_renderer().set_texture_filter(smooth ? nds::PolygonRenderer::TextureFilter::Bilinear
                                                        : nds::PolygonRenderer::TextureFilter::Nearest);
  // Vsync: adaptive where the system has it (a late picture is shown at
  // once rather than a whole refresh later), else plain. Without it the
  // window waits for the next step itself.
  const bool want_vsync = s.toggle(key::kVsync) || (workarounds_ & debug::kWorkaroundVsync) != 0;
  if (first || want_vsync != vsync_) {
    vsync_ = false;
    vsync_mode_ = "off";
    if (want_vsync && SDL_SetRenderVSync(app_.renderer, SDL_RENDERER_VSYNC_ADAPTIVE)) {
      vsync_ = true;
      vsync_mode_ = "adaptive";
    } else if (want_vsync && SDL_SetRenderVSync(app_.renderer, 1)) {
      vsync_ = true;
      vsync_mode_ = "every refresh";
    }
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
  if (first || scale != scale_applied_ || wide_ != was_wide) {
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
  update_music();
  audio_.play(mixer_.mix());
  keep_capture();
  if (!handle_events()) return false;
  if (menu_open_ && !run_menu()) return false;
  if (app_.settings.revision() != settings_seen_) apply_settings(false);
  read_input();
  // The scene is made during the frame, for the resolution it is drawn at.
  console_->polygon_renderer().set_fine_positions(app_.settings.toggle(key::kFineCoordinates) && scale_3d() > 1);
  // The HUD moves by a side's width, in whole DS pixels.
  console_->display().set_wide_hud(wide_hud_ ? Hardware3D::side_shift(scale_3d()) : 0);
  const bool capture_last = app_.options.frames != 0 && frames_run_ == app_.options.frames && !app_.options.capture.empty();
  const bool capture_now = capture_last || (app_.options.capture_every != 0 && !app_.options.capture.empty() &&
                                            frames_run_ >= app_.options.capture_from && frames_run_ % app_.options.capture_every == 0);
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
    if (first && capture_asked_) capture_this_frame();
    if (overlay_shown_) {
      frame_log::Scope frame_log_scope(frame_log::kFilter);
      draw_overlay();
    }
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
  if (!frame_log::enabled()) return;
  frame_log::State& s = frame_log::state();
  frame_log::switch_to(s.current);  // The times up to now.
  const double per_us = frame_log_ ? frame_log_->ticks_per_us() : ticks_per_us_;
  const nds::Display& display = console_->display();
  const bool hardware = hardware_.is_open() && !hardware_failed_ && ui::hardware_3d(app_.settings);
  if (row_names_.empty()) {
    std::vector<std::string>& names = row_names_;
    names = {"frame", "total_us"};
    for (const char* n : frame_log::kColumnNames) names.emplace_back(n);
    for (const char* n : {"arm9_instructions", "interpreted_instructions", "polygons", "vertices", "gpu_vertices", "scene_draws",
                          "scene_textures", "textures_decoded", "decoded_bytes", "textures_uploaded", "texture_bytes",
                          "screen_bytes", "video_bytes", "vertex_bytes", "bank_bytes", "raster_bytes", "gpu_captures", "pictures",
                          "timer_switches", "shown", "gpu_3d", "3d_top", "3d_bottom", "capture_made", "capture_shown",
                          "raster_ran", "edge_marking", "anti_aliasing", "fog", "scale", "main_on_top"})
      names.emplace_back(n);
  }
  if (frame_log_ && !frame_log_->started()) {
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
    frame_log_->start({line, second}, row_names_);
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
  if (frame_log_) frame_log_->row(v);
  row_values_ = v;
  if (perf_) {
    debug::PerfFrame f;
    f.frame = frames_run_;
    f.total_us = total / per_us;
    for (std::size_t i = 0; i < f.us.size(); ++i) f.us[i] = static_cast<float>(static_cast<double>(s.spent[i]) / per_us);
    f.polygons = static_cast<std::uint32_t>(c.polygons);
    f.vertices = static_cast<std::uint32_t>(c.vertices);
    f.shown = c.pictures != 0;
    f.top_3d = top_3d;
    f.bottom_3d = bottom_3d;
    f.capture_shown = captured(0) || captured(1);
    f.top_2d_gpu = display.screen_2d(0).by_hardware;
    f.bottom_2d_gpu = display.screen_2d(1).by_hardware;
    f.gpu_3d = hardware;
    if (c.textures_uploaded != 0) f.events |= debug::kEventAtlasRefill;
    if (c.targets_made != 0) f.events |= debug::kEventPipeline;
    if (c.gpu_captures != 0 || c.capture_made || captured_this_frame_) f.events |= debug::kEventCapture;
    perf_->add(f);
  }
  captured_this_frame_ = false;
  if (!capture_row_pending_.empty()) {
    write_capture_row(capture_row_pending_);
    capture_row_pending_.clear();
  }
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
        } else if (!debug_hotkey(event.key.scancode, SDL_GAMEPAD_BUTTON_INVALID)) {
          handle_window_key(event.key);
        }
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) menu_open_ = true;
        else if (debug_hotkey(SDL_SCANCODE_UNKNOWN, static_cast<SDL_GamepadButton>(event.gbutton.button))) break;
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
        if (perf_) perf_->note(frames_run_, "device_lost", "the graphics device was reset (SDL_EVENT_RENDER_DEVICE_RESET)");
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
  // The left stick pushed furthest, of all the controllers (or the script's).
  int stick_x = 0, stick_y = 0;
  const auto reach = [](int x, int y) { return static_cast<long long>(x) * x + static_cast<long long>(y) * y; };
  for (SDL_Gamepad* pad : gamepads_) {
    for (const PadButton& b : pad_buttons_)
      if (SDL_GetGamepadButton(pad, b.button)) held |= b.bit;
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (reach(x, y) > reach(stick_x, stick_y)) {
      stick_x = x;
      stick_y = y;
    }
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= input::kTriggerThreshold)
      held |= input::bit(input::DsButton::L);
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= input::kTriggerThreshold)
      held |= input::bit(input::DsButton::R);
  }
  if (const input::ScriptedStick* scripted = input::scripted_stick(app_.options.sticks, frames_run_)) {
    stick_x = scripted->x * 32767 / 100;
    stick_y = scripted->y * 32767 / 100;
  }

  // The stylus: the left button, on the touch screen, while the window has
  // the mouse. Off the screen is not touching it.
  bool touching = false;
  gfx::Point at;
  // The widescreen HUD's moved elements on the touch screen.
  std::vector<gfx::TouchMove> moves;
  if (wide_hud_ && layout_.wide_screen == 1) {
    const nds::HudPlan& plan = console_->display().hud_plan();
    for (unsigned i = 0; i < plan.touch_count; ++i) {
      const nds::HudPlan::Touch& t = plan.touches[i];
      moves.push_back({t.x0, t.y0, t.x1, t.y1, t.dx});
    }
  }
  if (const input::ScriptedTouch* scripted = input::scripted_touch(app_.options.touches, frames_run_)) {
    if (!scripted->wide) {
      touching = true;
      at = {scripted->x, scripted->y};
    } else if (const auto point = gfx::wide_touch(scripted->x + 0.5, scripted->y + 0.5, moves)) {
      touching = true;
      at = *point;
    }
  }
  float wx = 0, wy = 0;
  const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&wx, &wy);
  if (!touching && (buttons & SDL_BUTTON_LMASK) != 0 && SDL_GetMouseFocus() == app_.window) {
    float rx = 0, ry = 0;
    if (SDL_RenderCoordinatesFromWindow(app_.renderer, wx, wy, &rx, &ry)) {
      if (const auto point = gfx::touch_point(screens_, rx, ry, moves)) {
        touching = true;
        at = *point;
      }
    }
  }

  ConsoleMemory memory(*console_);
  game::StickWalk::Input walk;
  walk.held = held;
  walk.stick_x = stick_x;
  walk.stick_y = stick_y;
  walk.stick_directions = input::directions_from_stick(stick_x, stick_y);
  walk.touching = touching;
  walk.enabled = app_.settings.toggle(key::kAnalogWalking);
  held = stick_walk_.frame(memory, walk);
  if (stick_walk_.mode() != stick_mode_) {
    stick_mode_ = stick_walk_.mode();
    if (stick_mode_ == game::StickWalk::Mode::Walking) log::debug("input", "the left stick walks on the field");
    if (stick_mode_ == game::StickWalk::Mode::Pad) log::debug("input", "the left stick is the d-pad");
  }
  held = opening_skip_.frame(memory, held, app_.settings.toggle(key::kSkipOpening));
  if (opening_skip_.skipping() && !was_skipping_) log::info("game", "the opening is skipped (Start), up to the title");
  was_skipping_ = opening_skip_.skipping();
  if (held != console_->keypad().held()) console_->keypad().set_held(held);

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
      if (!display.screen_sides(i).empty()) picture.sides = &display.screen_sides(i);
    }
    bool failed = false;
    SDL_Texture* t = hardware_.draw(i, picture,
                                    fill.fill == nds::Display::Fill::kScene ? &console_->polygon_renderer().scene() : nullptr,
                                    fill.capture, &fill.lines, fill.brightness, scale_3d(), failed);
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
  std::size_t differ = 0, first = 0;
  for (std::size_t n = 0; n < pixels.size() && n < drawn.size(); ++n) {
    if (pixels[n] == nds::DisplayEngine::for_hardware(drawn[n], with_marks)) continue;
    if (differ++ == 0) first = n;
  }
  ++checked_2d_;
  if (differ == 0) return;
  if (++differed_2d_ > 20) return;
  const std::uint32_t* w = &display.screen_2d(i).lines[first / 256 * nds::Frame2D::kWords];
  const std::uint32_t wanted = nds::DisplayEngine::for_hardware(drawn[first], with_marks);
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
                         scale_3d(), display.kept_numbers(),
                         display.capture_sides().empty() ? nullptr : &display.capture_sides()))
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
  wide_ = false;
  hardware_.set_wide(false);
  console_->geometry().set_wide(false);
  console_->display().set_sides(false);
  wide_hud_ = false;
  console_->geometry().set_wide_hud(false);
  console_->display().set_wide_hud(0);
  layout_.wide_screen = -1;
  hardware_problem_ = std::string("the graphics hardware failed to ") + why;
  console_->display().hardware_3d_failed(hardware_problem_);
  if (perf_) perf_->note(frames_run_, "gpu_error", hardware_problem_);
}

int GameScreen::main_screen() const {
  return (console_->display().power_control() & nds::Display::kMainOnTop) != 0 ? 0 : 1;
}

SDL_Texture* GameScreen::enlarged(int i, SDL_Texture* source, int made_at, int factor) {
  const std::size_t n = static_cast<std::size_t>(i);
  SDL_Texture*& t = enlarged_[n];
  // As wide as the source, in the same proportion (a wide picture is wider).
  float source_w = 0, source_h = 0;
  SDL_GetTextureSize(source, &source_w, &source_h);
  const int width = static_cast<int>(source_w) / made_at * factor + static_cast<int>(source_w) % made_at * factor / made_at;
  if (factor != enlarged_factor_[n] || width != enlarged_width_[n]) {
    if (t != nullptr) SDL_DestroyTexture(t);
    t = SDL_CreateTexture(app_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width,
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
    enlarged_width_[n] = width;
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

// Pictures the main engine stays on one screen before that one is wide.
constexpr int kSteadyPictures = 30;

void GameScreen::draw() {
  int width = 0, height = 0;
  SDL_GetCurrentRenderOutputSize(app_.renderer, &width, &height);
  // With widescreen, the main engine's screen is the wide one, once it has
  // stayed there half a second: a game that shows 3D on both screens moves
  // it every frame, and both are then 4:3.
  const int main = main_screen();
  if (main != main_screen_seen_) {
    main_screen_seen_ = main;
    main_screen_pictures_ = 0;
  } else if (main_screen_pictures_ < kSteadyPictures) {
    ++main_screen_pictures_;
  }
  layout_.wide_screen = wide_ && main_screen_pictures_ >= kSteadyPictures ? main : -1;
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
    // By its lines: a wide picture is wider than 256 times that.
    const int made_at = std::max(1, static_cast<int>(h) / gfx::kScreenHeight);
    const int factor = gfx::enlarge_factor(filter_, screens_.scale, largest_texture_, made_at);
    if (factor == made_at) continue;  // Already a whole multiple at or above the scale.
    if (SDL_Texture* e = enlarged(static_cast<int>(i), shown[i], made_at, factor); e != nullptr) shown[i] = e;
  }
  SDL_SetRenderDrawColor(app_.renderer, 0, 0, 0, 255);
  SDL_RenderClear(app_.renderer);
  // A wide picture (the graphics hardware's, with widescreen) fills a wide
  // screen, and only its 256 columns in the middle show on one that is not;
  // a picture of the DS's size goes in the middle of a wide screen.
  // A graphics hardware picture larger than its place (made at 8x, shown
  // at 3x) is made smaller by averaging (Hardware3D::downscaled), but for
  // the Sharp filter: bilinear filtering alone would leave its edges
  // stepped.
  const auto place = [&](std::size_t i, SDL_Texture* texture, const gfx::Rect& r, bool wide_screen) {
    if (r.w <= 0 || r.h <= 0) return;  // Not shown (the touch screen alone).
    float w = 0, h = 0;
    SDL_GetTextureSize(texture, &w, &h);
    const float middle = h * gfx::kScreenWidth / gfx::kScreenHeight;  // The DS's screen's width in the picture.
    const bool wide_picture = w > middle + 0.5f;
    SDL_FRect to{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
    SDL_FRect from{0, 0, w, h};
    if (wide_screen && !wide_picture) {
      const float across = to.w * static_cast<float>(gfx::kScreenWidth / gfx::kWideWidth);
      to.x += (to.w - across) / 2;
      to.w = across;
    } else if (!wide_screen && wide_picture) {
      from.x = (w - middle) / 2;
      from.w = middle;
    }
    if (texture == shown_3d_[i] && filter_ != gfx::ScreenFilter::Sharp && from.h > to.h * 1.001f) {
      const float left = std::round(to.x), top = std::round(to.y);
      const int across = static_cast<int>(std::round(to.x + to.w) - left), down = static_cast<int>(std::round(to.y + to.h) - top);
      if (SDL_Texture* smaller = hardware_.downscaled(static_cast<int>(i), from.x, from.w, across, down)) {
        const SDL_FRect at{left, top, static_cast<float>(across), static_cast<float>(down)};
        SDL_RenderTexture(app_.renderer, smaller, nullptr, &at);
        return;
      }
    }
    SDL_RenderTexture(app_.renderer, texture, &from, &to);
  };
  place(0, shown[0], screens_.top, screens_.top_wide);
  place(1, shown[1], screens_.bottom, screens_.bottom_wide);
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
  captured_this_frame_ = true;
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

bool GameScreen::debug_hotkey(SDL_Scancode code, SDL_GamepadButton button) {
  for (const DebugKey& d : debug_keys_) {
    if (!((code != SDL_SCANCODE_UNKNOWN && d.code == code) || (button != SDL_GAMEPAD_BUTTON_INVALID && d.button == button)))
      continue;
    switch (d.hotkey) {
      case ui::Hotkey::PauseFrame:
        stepper_.pause();
        break;
      case ui::Hotkey::StepFrame:
        stepper_.step();
        break;
      case ui::Hotkey::ResumeFrame:
        if (stepper_.paused()) log::info("game", "frame by frame: runs on");
        stepper_.resume();
        break;
      case ui::Hotkey::FrameOverlay:
        overlay_shown_ = !overlay_shown_;
        break;
      case ui::Hotkey::CaptureFrame:
        capture_asked_ = true;
        break;
      default:
        break;
    }
    return true;
  }
  return false;
}

bool GameScreen::hold_if_paused() {
  if (!stepper_.after_frame(frames_run_)) return true;
  log::writef(log::Level::Debug, "game", "frame by frame: held after frame %" PRIu64, frames_run_);
  // Nothing of the game runs: no code, timing, sound or input. The window
  // is redrawn, from the screens as they are, and the hotkeys read.
  for (;;) {
    if (!handle_events()) return false;
    if (menu_open_ && !run_menu()) return false;
    if (app_.settings.revision() != settings_seen_) apply_settings(false);
    draw();
    if (capture_asked_) capture_this_frame();
    if (overlay_shown_) draw_overlay();
    SDL_RenderPresent(app_.renderer);
    if (stepper_.held_redrawn()) break;
    if (!vsync_) SDL_Delay(15);
  }
  // The time held is no frame's.
  clock_.restart(SDL_GetTicksNS());
  second_started_ = SDL_GetTicksNS();
  frames_this_second_ = pictures_this_second_ = 0;
  if (frame_log::enabled()) {
    frame_log::State& s = frame_log::state();
    s.since = frame_log::ticks();
    for (std::uint64_t& t : s.spent) t = 0;
    s.switches = 0;
  }
  return true;
}

void GameScreen::draw_overlay() {
  draw_frame_overlay(app_.renderer, frame_overlay_lines(frames_run_, row_names_, row_values_, gpu_.name, stepper_.paused()));
}

bool GameScreen::write_window(const std::filesystem::path& file) {
  SDL_Surface* surface = SDL_RenderReadPixels(app_.renderer, nullptr);
  if (surface == nullptr) {
    log::error("window", std::string("cannot read the window back: ") + SDL_GetError());
    return false;
  }
  const bool written = SDL_SaveBMP(surface, utf8(file).c_str());
  if (!written) log::error("window", "cannot write " + utf8(file) + ": " + SDL_GetError());
  SDL_DestroySurface(surface);
  return written;
}

void GameScreen::capture_this_frame() {
  capture_asked_ = false;
  captured_this_frame_ = true;
  if (app_.user_data.empty()) {
    log::warning("window", "capture this frame: there is no user data folder to write it to");
    return;
  }
  const std::filesystem::path folder = app_.user_data / "captures";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  const std::string name = "frame-" + std::to_string(frames_run_);
  if (write_window(folder / (name + ".bmp"))) log::info("window", "this frame was written to " + utf8(folder / (name + ".bmp")));
  // Its frame-log row: there if the game is held after it, else made at
  // the frame's end.
  const std::filesystem::path row = folder / (name + ".csv");
  if (!row_values_.empty() && static_cast<std::uint64_t>(row_values_[0]) == frames_run_) write_capture_row(row);
  else capture_row_pending_ = row;
}

void GameScreen::write_capture_row(const std::filesystem::path& file) {
  std::ofstream out(file, std::ios::binary);
  for (std::size_t i = 0; i < row_names_.size(); ++i) out << (i == 0 ? "" : ",") << row_names_[i];
  out << "\n";
  char text[32];
  for (std::size_t i = 0; i < row_values_.size(); ++i) {
    std::snprintf(text, sizeof text, "%.6g", row_values_[i]);
    out << (i == 0 ? "" : ",") << text;
  }
  out << "\n";
  if (!out) log::warning("window", "cannot write " + utf8(file));
}

void GameScreen::describe_gpu() {
  // The driver's own description (a Vulkan instance of its own) only where
  // it is used: the report, the overlay, or a table to match.
  const bool ask_driver = perf_ || app_.settings.toggle(key::kFrameOverlay) || !debug::known_gpu_issues().empty();
  gpu_ = app::describe_gpu(app_.gpu, ask_driver);
  if (app_.gpu == nullptr) return;
  const debug::DriverVersion version = debug::decode_driver_version(gpu_.vendor_id, gpu_.driver_id, gpu_.driver_version, gpu_.os);
  char ids[64] = "";
  if (gpu_.vendor_id != 0) std::snprintf(ids, sizeof ids, " (vendor 0x%04x, device 0x%04x)", gpu_.vendor_id, gpu_.device_id);
  log::info("window", "graphics card: " + (gpu_.name.empty() ? std::string("name unknown") : gpu_.name) + ids + ", " +
                          gpu_.backend + ", driver " +
                          (gpu_.driver_name.empty() ? gpu_.sdl_driver_name : gpu_.driver_name) + " " +
                          (gpu_.driver_version != 0 ? version.text() : gpu_.sdl_driver_version) +
                          (gpu_.driver_info.empty() ? gpu_.sdl_driver_info.empty() ? "" : " (" + gpu_.sdl_driver_info + ")"
                                                    : " (" + gpu_.driver_info + ")"));
  gpu_issues_ = debug::match_gpu_issues(gpu_, debug::known_gpu_issues());
  const bool apply = app_.settings.toggle(key::kGpuWorkarounds);
  for (const debug::GpuIssue* issue : gpu_issues_) {
    const std::string text = std::string("known GPU issue: ") + std::string(issue->problem) + "; workaround: " +
                             debug::workaround_names(issue->workarounds) + (apply ? "" : " (switched off on the Debug page)");
    log::warning("window", text);
    if (perf_) perf_->note(0, "known_gpu_issue", text);
  }
}

void GameScreen::write_perf_report() {
  if (!perf_) return;
  using Section = debug::PerfReport::Section;
  std::vector<Section> sections;
  sections.push_back(system_section());
  {
    Section g;
    g.name = "gpu";
    const debug::DriverVersion version = debug::decode_driver_version(gpu_.vendor_id, gpu_.driver_id, gpu_.driver_version, gpu_.os);
    const char* renderer = SDL_GetRendererName(app_.renderer);
    char hex[16];
    std::string summary = gpu_.backend.empty() ? std::string("no SDL GPU device; renderer ") + (renderer ? renderer : "none")
                                               : gpu_.backend + ": " + (gpu_.name.empty() ? "name unknown" : gpu_.name);
    if (!gpu_.device_type.empty()) summary += " (" + gpu_.device_type + ")";
    if (gpu_.driver_version != 0) summary += ", driver " + gpu_.driver_name + " " + version.text();
    else if (!gpu_.sdl_driver_version.empty()) summary += ", driver " + gpu_.sdl_driver_name + " " + gpu_.sdl_driver_version;
    if (gpu_.vram_bytes != 0) summary += ", " + std::to_string(gpu_.vram_bytes >> 20) + " MB";
    g.text("summary", summary);
    g.text("backend", gpu_.backend.empty() ? "none" : gpu_.backend);
    g.text("renderer", renderer != nullptr ? renderer : "none");
    g.text("name", gpu_.name);
    g.text("vendor", debug::vendor_name(gpu_.vendor_id));
    std::snprintf(hex, sizeof hex, "0x%04x", gpu_.vendor_id);
    g.text("vendor_id", hex);
    std::snprintf(hex, sizeof hex, "0x%04x", gpu_.device_id);
    g.text("device_id", hex);
    g.text("device_type", gpu_.device_type.empty() ? "unknown" : gpu_.device_type);
    g.number("driver_id", gpu_.driver_id);
    g.text("driver_name", gpu_.driver_name);
    g.text("driver_info", gpu_.driver_info);
    g.number("driver_version_raw", gpu_.driver_version);
    g.text("driver_version", gpu_.driver_version != 0 ? version.text() : "");
    g.text("driver_version_scheme", gpu_.driver_version != 0 ? version.scheme : "");
    g.text("api_version", gpu_.api_version != 0 ? debug::api_version_text(gpu_.api_version) : "");
    g.number("vram_mb", static_cast<double>(gpu_.vram_bytes >> 20));
    g.text("sdl_driver_name", gpu_.sdl_driver_name);
    g.text("sdl_driver_version", gpu_.sdl_driver_version);
    g.text("sdl_driver_info", gpu_.sdl_driver_info);
    std::string fallbacks;
    for (const std::string& n : app_.start_notes) fallbacks += (fallbacks.empty() ? "" : "; ") + n;
    g.text("start_fallbacks", fallbacks);
    g.text("hardware_3d_problem", hardware_problem_);
    sections.push_back(std::move(g));
  }
  {
    Section k;
    k.name = "known_gpu_issues";
    const bool apply = app_.settings.toggle(key::kGpuWorkarounds);
    k.text("summary", gpu_issues_.empty() ? "no known GPU issue matches"
                                          : std::to_string(gpu_issues_.size()) + " known GPU issue(s) match; workarounds " +
                                                (apply ? debug::workaround_names(workarounds_) : "switched off"));
    k.number("matched", static_cast<double>(gpu_issues_.size()));
    k.flag("workarounds_on", apply);
    k.text("workarounds_applied", debug::workaround_names(workarounds_));
    for (std::size_t i = 0; i < gpu_issues_.size(); ++i)
      k.text("issue_" + std::to_string(i + 1), std::string(gpu_issues_[i]->problem) + " -> " +
                                                   debug::workaround_names(gpu_issues_[i]->workarounds));
    sections.push_back(std::move(k));
  }
  {
    const platform::Settings& st = app_.settings;
    Section t;
    t.name = "settings";
    // As the last frame had them: the console may be gone by now.
    const bool hardware = hardware_.is_open() && !hardware_failed_ && ui::hardware_3d(st);
    const debug::PerfFrame last = perf_->frames() != 0 ? perf_->all().back() : debug::PerfFrame{};
    const auto path_2d = [&](int i) { return (i == 0 ? last.top_2d_gpu : last.bottom_2d_gpu) ? "gpu" : "processor"; };
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(app_.window, &w, &h);
    const char* filter = filter_ == gfx::ScreenFilter::SharpSmoothEdges ? "sharp pixels, smooth edges"
                         : filter_ == gfx::ScreenFilter::Smooth         ? "smooth"
                                                                        : "sharp";
    char summary[512];
    std::snprintf(summary, sizeof summary,
                  "scale %lld, 3D %dx (%s), widescreen %s (HUD %s), 3D by %s, 2D top %s / bottom %s, filter %s, "
                  "textures %s, window %d x %d%s, vsync %s",
                  st.number(key::kScale), hardware ? scale_3d() : 1, st.choice(key::k3DResolution).c_str(),
                  wide_ ? "on" : "off", st.choice(key::kWidescreenHud).c_str(), hardware ? "gpu" : "processor", path_2d(0),
                  path_2d(1), filter, ui::smooth_textures(st) ? "smooth" : "sharp", w, h,
                  fullscreen_applied_ ? " full screen" : "", vsync_mode_.c_str());
    t.text("summary", summary);
    t.number("scale", static_cast<double>(st.number(key::kScale)));
    t.text("resolution_3d_setting", st.choice(key::k3DResolution));
    t.number("resolution_3d", hardware ? scale_3d() : 1);
    t.flag("widescreen", wide_);
    t.text("widescreen_hud", st.choice(key::kWidescreenHud));
    t.text("renderer_3d", hardware ? "gpu" : "processor");
    t.text("renderer_2d_top", path_2d(0));
    t.text("renderer_2d_bottom", path_2d(1));
    t.text("filter", filter);
    t.text("textures", ui::smooth_textures(st) ? "smooth" : "sharp");
    t.text("arrangement", st.choice(key::kArrangement));
    t.number("window_width", w);
    t.number("window_height", h);
    t.flag("fullscreen", fullscreen_applied_);
    t.flag("vsync", vsync_);
    t.text("present_mode", vsync_mode_);
    t.flag("unthrottled", app_.options.unthrottled);
    t.flag("frame_log_sync", frame_log::state().sync);
    t.text("code", !app_.native_code ? "interpreted" : app_.translated_build ? "translated" : "built game");
    sections.push_back(std::move(t));
  }
  const std::string text = perf_->json(sections, frame_log::state().sync, vsync_);
  std::ofstream out(perf_path_, std::ios::binary);
  out << text;
  if (out) {
    log::info("frames", "the performance report was written to " + utf8(perf_path_));
    std::printf("The performance report was written to %s\n", utf8(perf_path_).c_str());
  } else {
    log::warning("frames", "cannot write the performance report to " + utf8(perf_path_));
  }
  perf_.reset();
}

}  // namespace bd::app
