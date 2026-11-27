// Bright Dawn's settings: each one described once, here. The settings book
// (the window program's settings screens) is made from these descriptions,
// and so is the settings file, so a setting added here appears in both.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gfx/screen_layout.h"
#include "input/bindings.h"
#include "nds/hardware_scene.h"
#include "nds/user_settings.h"
#include "platform/settings.h"

namespace bd::ui {

// The keys.
namespace key {
inline constexpr std::string_view kScale = "display.scale";
inline constexpr std::string_view kArrangement = "display.arrangement";
inline constexpr std::string_view kPreviousArrangement = "display.previous_arrangement";
inline constexpr std::string_view kTouchScreenFirst = "display.touch_screen_first";
inline constexpr std::string_view kGap = "display.gap";
inline constexpr std::string_view kScaling = "display.scaling";
inline constexpr std::string_view kScreenFilter = "display.filter";
inline constexpr std::string_view kFullscreen = "display.fullscreen";
inline constexpr std::string_view kVsync = "display.vsync";
inline constexpr std::string_view kRenderer = "display.renderer";
inline constexpr std::string_view kTextures = "graphics.textures";
inline constexpr std::string_view k3DDrawing = "graphics.drawing_3d";
inline constexpr std::string_view k3DResolution = "graphics.resolution_3d";
inline constexpr std::string_view kFineCoordinates = "graphics.fine_coordinates";
inline constexpr std::string_view kEdgeMarking = "graphics.edge_marking";
inline constexpr std::string_view kWidescreen = "graphics.widescreen";
inline constexpr std::string_view kWidescreenHud = "graphics.widescreen_hud";
inline constexpr std::string_view kSound = "sound.enabled";
inline constexpr std::string_view kVolume = "sound.volume";
inline constexpr std::string_view kPlayBuilt = "game.play_built";
inline constexpr std::string_view kLanguage = "game.language";
inline constexpr std::string_view kRom = "game.rom";
inline constexpr std::string_view kSkipOpening = "quality_of_life.skip_opening";
inline constexpr std::string_view kAnalogWalking = "controls.analog_walking";
inline constexpr std::string_view kBuildJobs = "builder.jobs";
inline constexpr std::string_view kBuildCompiler = "builder.compiler";
inline constexpr std::string_view kLogLevel = "log.level";
inline constexpr std::string_view kLogsKept = "log.kept";
// The launcher's Debug page: not in the settings book.
inline constexpr std::string_view kFrameLog = "debug.frame_log";
inline constexpr std::string_view kFrameLogSync = "debug.frame_log_sync";
inline constexpr std::string_view kProcessor2D = "debug.processor_2d";
inline constexpr std::string_view kPauseStep = "debug.pause_step";
inline constexpr std::string_view kFrameOverlay = "debug.frame_overlay";
inline constexpr std::string_view kCaptureHotkey = "debug.capture_hotkey";
inline constexpr std::string_view kPerfReport = "debug.perf_report";
inline constexpr std::string_view kGpuWorkarounds = "debug.gpu_workarounds";
}  // namespace key

std::vector<platform::SettingDef> game_setting_definitions();

// The Controls chapter of the settings book: its settings, then the
// bindings, drawn by a page of their own (app/controls_page.h) rather than
// from the definitions.
inline constexpr std::string_view kControlsChapter = "Controls";

// Something the window does, bound like a DS button.
enum class Hotkey {
  None,              // A DS button.
  BottomScreenOnly,  // The touch screen alone, or back to the screens before.
  // Debug (launcher, Debug page), each only while its setting is on.
  PauseFrame,    // Pause and step: hold the game after the current frame.
  StepFrame,     // While held: run exactly one frame.
  ResumeFrame,   // Run on.
  FrameOverlay,  // The frame overlay on or off.
  CaptureFrame,  // Capture this frame: the picture and its frame-log row.
};

// What can be bound on the Controls page: a DS button or a hotkey, to a key
// and to a controller button. Each binding is a setting of its own, kept in
// the settings file like the others: the control's name as SDL names it
// ("Right Shift", "dpup"), or empty for none.
struct Control {
  std::string label;
  std::string keyboard_key;    // "controls.keyboard.<name>"
  std::string controller_key;  // "controls.controller.<name>"
  input::DsButton button = input::DsButton::A;  // When hotkey is None.
  Hotkey hotkey = Hotkey::None;
};
const std::vector<Control>& controls();
// The bindings of DS buttons the settings give: one for each bound.
std::vector<input::Binding> keyboard_bindings(const platform::Settings& settings);
std::vector<input::Binding> controller_bindings(const platform::Settings& settings);
// The key or controller button bound to a hotkey, as SDL names it; empty
// for none.
std::string hotkey_binding(const platform::Settings& settings, Hotkey hotkey, bool keyboard);
// What the BottomScreenOnly hotkey sets, in order: to the touch screen
// alone, remembering the arrangement before; or from it, back to that one.
std::vector<std::pair<std::string, std::string>> bottom_screen_switch(const platform::Settings& settings);
// The keys of the bindings (keyboard or controller) whose control is also
// bound to another DS button.
std::vector<std::string> bound_twice(const platform::Settings& settings, bool keyboard);
// Every binding back to its default.
void reset_controls(platform::Settings& settings);

// The screens' arrangement the settings ask for.
gfx::LayoutOptions layout_options(const platform::Settings& settings);
// How the screens are scaled to the window.
gfx::ScreenFilter screen_filter(const platform::Settings& settings);
// Whether the 3D scenes' textures are to be smoothed (bilinear) rather than
// sharp (nearest, as on the DS).
bool smooth_textures(const platform::Settings& settings);
// Whether the graphics hardware is to draw the 3D scenes.
bool hardware_3d(const platform::Settings& settings);
// How many times the DS's resolution the graphics hardware draws 3D scenes
// at (1 to 8), for a screen shown `shown_height` pixels high: as set, or
// for "auto" the least that is at least as high, so no detail is lost.
int resolution_3d(const platform::Settings& settings, int shown_height);
// The outlines the game asks for (edge marking): off, one DS pixel wide at
// any resolution, or one pixel of the resolution drawn at.
nds::EdgeMarking edge_marking(const platform::Settings& settings);
// The language the console is set to, which the game reads at its start.
nds::UserSettings::Language game_language(const platform::Settings& settings);

// Where the settings are kept: settings.txt in the user data directory.
std::filesystem::path settings_file(const std::filesystem::path& user_data);

}  // namespace bd::ui
