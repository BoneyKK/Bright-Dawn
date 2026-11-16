// Bright Dawn: the game in a window.
//
// Loads the game from the user's ROM (or from code extracted from it), starts
// the console model and shows its two screens, a frame at a time, at the
// console's rate. The keyboard and any number of game controllers press the
// DS's buttons; the left mouse button on the touch screen is the stylus.
//
// The game's code is interpreted for now: no translated code is linked in.
//
// Keys of the window itself: F11 or Alt+Enter, full screen on and off; Tab,
// the screens one above the other or side by side; F12, whole-number scaling
// or filling the window.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/version.h"
#include "gfx/screen_image.h"
#include "gfx/screen_layout.h"
#include "input/bindings.h"
#include "nds/console.h"
#include "nds/keypad.h"
#include "nds/program.h"
#include "platform/frame_pacer.h"
#include "rom/game_id.h"
#include "rom/rom.h"
#include "translated_table.h"

namespace {

const TranslatedEntry kNoTranslations[] = {{0, nullptr, -1, nullptr, 0}};

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
    "Usage: bright-dawn [options] <ROM file, or a directory extracted from it>\n"
    "\n"
    "Options:\n"
    "  --scale N             start with the screens N times their size (default 2)\n"
    "  --side-by-side        the screens side by side instead of one above the other\n"
    "  --gap N               N DS pixels between the screens (default 0)\n"
    "  --touch-screen-first  the touch screen above (or left of) the other\n"
    "  --fit                 fill the window instead of scaling by whole numbers\n"
    "  --fullscreen          start full screen\n"
    "  --unthrottled         run as fast as possible instead of at the console's rate\n"
    "  --frames N            stop after N frames\n"
    "  --hold BUTTON@F[-L]   hold a DS button from frame F to frame L (counting from 1),\n"
    "                        as if pressed; may be given more than once\n"
    "  --touch X,Y@F[-L]     touch the touch screen at pixel X,Y from frame F to frame L;\n"
    "                        may be given more than once\n"
    "  --capture FILE        with --frames, write the window's last frame to FILE (BMP)\n"
    "  --video-drivers       list the window systems this build can use, and stop\n"
    "  --version             print the version, and stop\n"
    "\n"
    "Keyboard: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start,\n"
    "Right Shift = Select. Controllers: by position, as on the DS. The left mouse\n"
    "button on the touch screen is the stylus. F11 or Alt+Enter: full screen.\n"
    "Tab: stacked or side by side. F12: whole-number scaling or fill.\n";

struct Options {
  std::filesystem::path source;
  int scale = 2;
  bd::gfx::LayoutOptions layout;
  bool fullscreen = false;
  bool unthrottled = false;
  std::uint64_t frames = 0;
  std::string capture;
  std::vector<bd::input::ScriptedPress> script;
  std::vector<bd::input::ScriptedTouch> touches;
};

// Command-line arguments arrive as UTF-8 (SDL_main sees to that on Windows).
std::filesystem::path path_from_utf8(const char* text) {
  const std::string s(text);
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool parse_number(const char* text, long long low, long long high, long long& out) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0' || value < low || value > high) return false;
  out = value;
  return true;
}

// Shown in a message box as well as printed, since on Windows the window
// program has no console to print to.
void complain(SDL_Window* window, const std::string& message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bright Dawn", message.c_str(), window);
}

class Window {
 public:
  Window(const Options& options, bd::nds::Console& console) : options_(options), console_(console) {}
  ~Window() {
    for (SDL_Gamepad* pad : gamepads_) SDL_CloseGamepad(pad);
    if (top_ != nullptr) SDL_DestroyTexture(top_);
    if (bottom_ != nullptr) SDL_DestroyTexture(bottom_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
  }

  bool open(std::string& error) {
    layout_ = options_.layout;
    const bd::gfx::Point size = bd::gfx::window_size(options_.scale, layout_);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (options_.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    title_ = std::string("Bright Dawn ") + BD_VERSION;
    if (!SDL_CreateWindowAndRenderer(title_.c_str(), size.x, size.y, flags, &window_, &renderer_)) {
      error = std::string("Cannot open a window: ") + SDL_GetError();
      return false;
    }
    SDL_SetWindowMinimumSize(window_, bd::gfx::kScreenWidth / 2, bd::gfx::kScreenHeight / 2);
    for (SDL_Texture** texture : {&top_, &bottom_}) {
      *texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                   bd::gfx::kScreenWidth, bd::gfx::kScreenHeight);
      if (*texture == nullptr) {
        error = std::string("Cannot make a texture for the screens: ") + SDL_GetError();
        return false;
      }
    }
    set_scale_mode();
    for (const bd::input::Binding& b : bd::input::default_keyboard_bindings()) {
      const SDL_Scancode code = SDL_GetScancodeFromName(b.control.c_str());
      if (code != SDL_SCANCODE_UNKNOWN) keys_.push_back({code, bd::input::bit(b.button)});
      else std::fprintf(stderr, "warning: no key is called \"%s\"\n", b.control.c_str());
    }
    for (const bd::input::Binding& b : bd::input::default_controller_bindings()) {
      const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(b.control.c_str());
      if (button != SDL_GAMEPAD_BUTTON_INVALID) pad_buttons_.push_back({button, bd::input::bit(b.button)});
      else std::fprintf(stderr, "warning: no controller button is called \"%s\"\n", b.control.c_str());
    }
    second_started_ = SDL_GetTicksNS();
    return true;
  }

  // At each frame: the screens shown, the input read, time kept. False when
  // the window was closed.
  bool frame() {
    if (!handle_events()) return false;
    read_input();
    draw();
    ++frames_shown_;
    if (options_.frames != 0 && frames_shown_ == options_.frames && !options_.capture.empty()) capture();
    SDL_RenderPresent(renderer_);
    count_frames_per_second();
    if (!options_.unthrottled) {
      const std::uint64_t next = pacer_.frame_shown(SDL_GetTicksNS());
      const std::uint64_t now = SDL_GetTicksNS();
      if (next > now) SDL_DelayPrecise(next - now);
    }
    return true;
  }

  SDL_Window* window() const { return window_; }
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

  void set_scale_mode() {
    // Whole-number scaling keeps every DS pixel square and sharp; filling
    // the window smooths the uneven pixels it would otherwise make.
    const SDL_ScaleMode mode =
        layout_.scaling == bd::gfx::Scaling::WholeNumbers ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR;
    SDL_SetTextureScaleMode(top_, mode);
    SDL_SetTextureScaleMode(bottom_, mode);
  }

  bool handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          return false;
        case SDL_EVENT_KEY_DOWN:
          if (event.key.repeat) break;
          if (event.key.scancode == SDL_SCANCODE_F11 ||
              (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0)) {
            const bool full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(window_, !full);
          } else if (event.key.scancode == SDL_SCANCODE_TAB) {
            layout_.arrangement = layout_.arrangement == bd::gfx::Arrangement::Stacked ? bd::gfx::Arrangement::SideBySide
                                                                                       : bd::gfx::Arrangement::Stacked;
          } else if (event.key.scancode == SDL_SCANCODE_F12) {
            layout_.scaling = layout_.scaling == bd::gfx::Scaling::WholeNumbers ? bd::gfx::Scaling::Fit
                                                                                : bd::gfx::Scaling::WholeNumbers;
            set_scale_mode();
          }
          break;
        case SDL_EVENT_GAMEPAD_ADDED:
          if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) gamepads_.push_back(pad);
          break;
        case SDL_EVENT_GAMEPAD_REMOVED:
          for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it) {
            if (SDL_GetGamepadID(*it) == event.gdevice.which) {
              SDL_CloseGamepad(*it);
              gamepads_.erase(it);
              break;
            }
          }
          break;
        default:
          break;
      }
    }
    return true;
  }

  void read_input() {
    // The script's buttons for the frame that is ending: the first call is
    // frame 1.
    std::uint32_t held = bd::input::scripted_buttons(options_.script, frames_shown_ + 1);
    // Keys only count while the window has the keyboard: SDL clears its key
    // state when the focus goes.
    int key_count = 0;
    const bool* keys = SDL_GetKeyboardState(&key_count);
    for (const Key& key : keys_)
      if (static_cast<int>(key.code) < key_count && keys[key.code]) held |= key.bit;
    for (SDL_Gamepad* pad : gamepads_) {
      for (const PadButton& b : pad_buttons_)
        if (SDL_GetGamepadButton(pad, b.button)) held |= b.bit;
      held |= bd::input::directions_from_stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                                               SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
      if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= bd::input::kTriggerThreshold)
        held |= bd::input::bit(bd::input::DsButton::L);
      if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= bd::input::kTriggerThreshold)
        held |= bd::input::bit(bd::input::DsButton::R);
    }
    if (held != console_.keypad().held()) console_.keypad().set_held(held);

    // The stylus: the left button, on the touch screen, while the window has
    // the mouse. Off the screen is not touching it.
    bool touching = false;
    bd::gfx::Point at;
    if (const bd::input::ScriptedTouch* scripted = bd::input::scripted_touch(options_.touches, frames_shown_ + 1)) {
      touching = true;
      at = {scripted->x, scripted->y};
    }
    float wx = 0, wy = 0;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&wx, &wy);
    if (!touching && (buttons & SDL_BUTTON_LMASK) != 0 && SDL_GetMouseFocus() == window_) {
      float rx = 0, ry = 0;
      if (SDL_RenderCoordinatesFromWindow(renderer_, wx, wy, &rx, &ry)) {
        if (const auto point = bd::gfx::touch_point(screens_, rx, ry)) {
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

  void draw() {
    int width = 0, height = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &width, &height);
    screens_ = bd::gfx::lay_out(width, height, layout_);
    const auto upload = [&](SDL_Texture* texture, const std::vector<std::uint32_t>& screen) {
      if (screen.size() != std::size_t{bd::gfx::kScreenWidth} * bd::gfx::kScreenHeight) return;
      bd::gfx::to_rgba_bytes(screen.data(), screen.size(), pixels_.data());
      SDL_UpdateTexture(texture, nullptr, pixels_.data(), bd::gfx::kScreenWidth * 4);
    };
    upload(top_, console_.display().top_screen());
    upload(bottom_, console_.display().bottom_screen());
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const auto place = [&](SDL_Texture* texture, const bd::gfx::Rect& r) {
      const SDL_FRect to{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                         static_cast<float>(r.h)};
      SDL_RenderTexture(renderer_, texture, nullptr, &to);
    };
    place(top_, screens_.top);
    place(bottom_, screens_.bottom);
  }

  void capture() {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    if (surface == nullptr) {
      std::fprintf(stderr, "error: cannot read the window back: %s\n", SDL_GetError());
      return;
    }
    if (SDL_SaveBMP(surface, options_.capture.c_str())) {
      captured_ = true;
      std::printf("The last frame was written to %s\n", options_.capture.c_str());
    } else {
      std::fprintf(stderr, "error: cannot write %s: %s\n", options_.capture.c_str(), SDL_GetError());
    }
    SDL_DestroySurface(surface);
  }

  // The window's title shows how many frames a second are shown, so that a
  // machine too slow for the console's rate shows it.
  void count_frames_per_second() {
    ++frames_this_second_;
    const std::uint64_t now = SDL_GetTicksNS();
    if (now - second_started_ < 1'000'000'000ull) return;
    const double rate = static_cast<double>(frames_this_second_) * 1e9 / static_cast<double>(now - second_started_);
    char text[32];
    std::snprintf(text, sizeof text, " - %.1f frames/s", rate);
    SDL_SetWindowTitle(window_, (title_ + text).c_str());
    second_started_ = now;
    frames_this_second_ = 0;
  }

  const Options& options_;
  bd::nds::Console& console_;
  bd::gfx::LayoutOptions layout_;
  bd::gfx::ScreenLayout screens_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* top_ = nullptr;
  SDL_Texture* bottom_ = nullptr;
  std::vector<SDL_Gamepad*> gamepads_;
  std::vector<Key> keys_;
  std::vector<PadButton> pad_buttons_;
  std::array<std::uint8_t, bd::gfx::kScreenWidth * bd::gfx::kScreenHeight * 4> pixels_{};
  bool touching_ = false;
  bd::gfx::Point touch_at_;
  bd::platform::FramePacer pacer_;
  std::string title_;
  std::uint64_t frames_shown_ = 0, frames_this_second_ = 0, second_started_ = 0;
  bool captured_ = false;
};

int run(const Options& options) {
  using namespace bd;
  std::error_code ec;
  Result<nds::Program> loaded = Error{ErrorCode::Io, "not loaded"};
  Result<rom::Rom> rom = Error{ErrorCode::Io, "no ROM"};
  if (std::filesystem::is_directory(options.source, ec)) {
    loaded = nds::program_from_directory(options.source);
  } else {
    rom = rom::Rom::open(options.source);
    if (!rom) {
      complain(nullptr, rom.error().message);
      return 1;
    }
    if (auto status = rom::require_dark_dawn(rom.value().header()); !status) {
      complain(nullptr, status.error().message);
      return 1;
    }
    loaded = nds::program_from_rom(rom.value());
  }
  if (!loaded) {
    complain(nullptr, loaded.error().message);
    return 1;
  }

  nds::Console console(loaded.value(), kNoTranslations);
  if (rom) console.insert_cartridge(rom.value().bytes());
  if (auto status = console.direct_boot(); !status) {
    complain(nullptr, status.error().message);
    return 1;
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    complain(nullptr, std::string("Cannot start SDL: ") + SDL_GetError());
    return 1;
  }
  int status = 0;
  {
    Window window(options, console);
    std::string error;
    if (!window.open(error)) {
      complain(nullptr, error);
      status = 1;
    } else {
      console.on_frame = [&window] { return window.frame(); };
      const nds::RunResult result = console.run(0, options.frames);
      console.on_frame = nullptr;
      std::printf("%" PRIu64 " frames shown\n", console.display().statistics().frames);
      if (result.reason == nds::StopReason::Unhandled) {
        char where[16];
        std::snprintf(where, sizeof where, "0x%08" PRIX32, result.address);
        complain(window.window(), std::string("The game stopped at ") + where + ": it needed something Bright Dawn "
                                      "does not do yet.\n\n" + result.detail);
        status = 2;
      } else if (result.reason == nds::StopReason::Returned) {
        complain(window.window(), "The game's program ended.");
        status = 2;
      }
      if (!options.capture.empty() && !window.captured()) status = 1;
    }
  }
  SDL_Quit();
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  bool have_source = false;
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
    } else if (arg == "--scale" && has_value && parse_number(argv[i + 1], 1, 16, number)) {
      options.scale = static_cast<int>(number);
      ++i;
    } else if (arg == "--gap" && has_value && parse_number(argv[i + 1], 0, 256, number)) {
      options.layout.gap = static_cast<int>(number);
      ++i;
    } else if (arg == "--frames" && has_value && parse_number(argv[i + 1], 1, 1LL << 40, number)) {
      options.frames = static_cast<std::uint64_t>(number);
      ++i;
    } else if (arg == "--hold" && has_value) {
      const auto press = bd::input::parse_scripted_press(argv[i + 1]);
      if (!press) {
        complain(nullptr, std::string("Not a button and frames: ") + argv[i + 1] + "\n\n" + kUsage);
        return 1;
      }
      options.script.push_back(*press);
      ++i;
    } else if (arg == "--touch" && has_value) {
      const auto touch = bd::input::parse_scripted_touch(argv[i + 1]);
      if (!touch) {
        complain(nullptr, std::string("Not a point and frames: ") + argv[i + 1] + "\n\n" + kUsage);
        return 1;
      }
      options.touches.push_back(*touch);
      ++i;
    } else if (arg == "--capture" && has_value) {
      options.capture = argv[++i];
    } else if (arg == "--side-by-side") {
      options.layout.arrangement = bd::gfx::Arrangement::SideBySide;
    } else if (arg == "--touch-screen-first") {
      options.layout.bottom_first = true;
    } else if (arg == "--fit") {
      options.layout.scaling = bd::gfx::Scaling::Fit;
    } else if (arg == "--fullscreen") {
      options.fullscreen = true;
    } else if (arg == "--unthrottled") {
      options.unthrottled = true;
    } else if (!arg.empty() && arg[0] != '-' && !have_source) {
      options.source = path_from_utf8(argv[i]);
      have_source = true;
    } else {
      complain(nullptr, "Not understood: " + arg + "\n\n" + kUsage);
      return 1;
    }
  }
  if (!have_source) {
    complain(nullptr, std::string("Which ROM? Give the path of your Golden Sun: Dark Dawn ROM.\n\n") + kUsage);
    return 1;
  }
  if (!options.capture.empty() && options.frames == 0) {
    complain(nullptr, "--capture needs --frames: it writes the last of them.");
    return 1;
  }
  return run(options);
}
