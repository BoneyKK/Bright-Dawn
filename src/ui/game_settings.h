// Bright Dawn's settings: each one described once, here. The settings book
// (the window program's settings screens) is made from these descriptions,
// and so is the settings file, so a setting added here appears in both.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "gfx/screen_layout.h"
#include "platform/settings.h"

namespace bd::ui {

// The keys.
namespace key {
inline constexpr std::string_view kScale = "display.scale";
inline constexpr std::string_view kArrangement = "display.arrangement";
inline constexpr std::string_view kTouchScreenFirst = "display.touch_screen_first";
inline constexpr std::string_view kGap = "display.gap";
inline constexpr std::string_view kScaling = "display.scaling";
inline constexpr std::string_view kScreenFilter = "display.filter";
inline constexpr std::string_view kFullscreen = "display.fullscreen";
inline constexpr std::string_view kVsync = "display.vsync";
inline constexpr std::string_view kTextures = "graphics.textures";
inline constexpr std::string_view kPlayBuilt = "game.play_built";
inline constexpr std::string_view kRom = "game.rom";
inline constexpr std::string_view kBuildJobs = "builder.jobs";
inline constexpr std::string_view kLogLevel = "log.level";
inline constexpr std::string_view kLogsKept = "log.kept";
}  // namespace key

std::vector<platform::SettingDef> game_setting_definitions();

// The screens' arrangement the settings ask for.
gfx::LayoutOptions layout_options(const platform::Settings& settings);
// How the screens are scaled to the window.
gfx::ScreenFilter screen_filter(const platform::Settings& settings);
// Whether the 3D scenes' textures are to be smoothed (bilinear) rather than
// sharp (nearest, as on the DS).
bool smooth_textures(const platform::Settings& settings);

// Where the settings are kept: settings.txt in the user data directory.
std::filesystem::path settings_file(const std::filesystem::path& user_data);

}  // namespace bd::ui
