#include "ui/game_settings.h"

#include <string>

namespace bd::ui {

using platform::SettingDef;
using platform::SettingKind;

std::vector<SettingDef> game_setting_definitions() {
  const auto toggle = [](std::string_view k, const char* chapter, const char* label, const char* help, bool on) {
    SettingDef d;
    d.key = std::string(k);
    d.chapter = chapter;
    d.label = label;
    d.help = help;
    d.kind = SettingKind::Toggle;
    d.default_value = on ? "on" : "off";
    return d;
  };
  const auto number = [](std::string_view k, const char* chapter, const char* label, const char* help, long long low,
                         long long high, long long value) {
    SettingDef d;
    d.key = std::string(k);
    d.chapter = chapter;
    d.label = label;
    d.help = help;
    d.kind = SettingKind::Number;
    d.low = low;
    d.high = high;
    d.default_value = std::to_string(value);
    return d;
  };
  const auto choice = [](std::string_view k, const char* chapter, const char* label, const char* help,
                         std::vector<platform::SettingChoice> choices, const char* value) {
    SettingDef d;
    d.key = std::string(k);
    d.chapter = chapter;
    d.label = label;
    d.help = help;
    d.kind = SettingKind::Choice;
    d.choices = std::move(choices);
    d.default_value = value;
    return d;
  };

  std::vector<SettingDef> defs;
  defs.push_back(number(key::kScale, "Display", "Window size",
                        "How many times the DS's size the window opens at.", 1, 8, 2));
  defs.push_back(choice(key::kArrangement, "Display", "The two screens", "How the DS's two screens are placed.",
                        {{"stacked", "One above the other"}, {"side_by_side", "Side by side"}}, "stacked"));
  defs.push_back(toggle(key::kTouchScreenFirst, "Display", "Touch screen first",
                        "The touch screen above (or left of) the other, instead of below (or right of) it.", false));
  defs.push_back(number(key::kGap, "Display", "Gap between the screens", "In DS pixels.", 0, 96, 0));
  defs.push_back(choice(key::kScaling, "Display", "Scaling",
                        "Whole numbers keep every DS pixel the same size, with a border where the window is "
                        "larger. Filling the window uses all of it; some pixels are then a little larger than "
                        "others. Either way pixels stay sharp.",
                        {{"whole", "Whole numbers"}, {"fill", "Fill the window"}}, "whole"));
  defs.push_back(toggle(key::kFullscreen, "Display", "Full screen", "Start full screen (F11 changes it).", false));
  defs.push_back(toggle(key::kVsync, "Display", "Wait for the screen's refresh",
                        "Show each picture when the screen refreshes (vsync): no tearing.", true));
  defs.push_back(choice(key::kTextures, "Graphics", "Textures in 3D scenes",
                        "Sharp shows each texel as a block of pixels, as the DS does. Smooth mixes neighbouring "
                        "texels, for softer surfaces. Only the 3D scenes change: the HUD, menus, text and "
                        "everything 2D always stay sharp.",
                        {{"sharp", "Sharp (nearest)"}, {"smooth", "Smooth (bilinear)"}}, "sharp"));
  defs.push_back(toggle(key::kPlayBuilt, "Game", "Play the built game",
                        "When the game has been built from your ROM, play that: its code runs natively, much "
                        "faster than the interpreter.",
                        true));
  SettingDef rom;
  rom.key = std::string(key::kRom);
  rom.label = "ROM";
  rom.help = "The ROM last chosen.";
  rom.kind = SettingKind::Text;
  defs.push_back(rom);
  defs.push_back(number(key::kBuildJobs, "Builder", "Parallel jobs",
                        "How many files the builder compiles at once; 0 for as many as there are processors. "
                        "Fewer needs less memory.",
                        0, 256, 0));
  defs.push_back(choice(key::kLogLevel, "Log", "Detail",
                        "How much the log records. Each run writes a log; when reporting a problem, attach it.",
                        {{"error", "Errors"}, {"warning", "Warnings"}, {"info", "What happens (normal)"},
                         {"debug", "In detail"}},
                        "info"));
  defs.push_back(number(key::kLogsKept, "Log", "Logs kept", "The logs of this many runs are kept; older ones are "
                        "removed.", 1, 100, 10));
  return defs;
}

gfx::LayoutOptions layout_options(const platform::Settings& settings) {
  gfx::LayoutOptions options;
  options.arrangement = settings.choice(key::kArrangement) == "side_by_side" ? gfx::Arrangement::SideBySide
                                                                             : gfx::Arrangement::Stacked;
  options.scaling = settings.choice(key::kScaling) == "fill" ? gfx::Scaling::Fit : gfx::Scaling::WholeNumbers;
  options.gap = static_cast<int>(settings.number(key::kGap));
  options.bottom_first = settings.toggle(key::kTouchScreenFirst);
  return options;
}

bool smooth_textures(const platform::Settings& settings) { return settings.choice(key::kTextures) == "smooth"; }

std::filesystem::path settings_file(const std::filesystem::path& user_data) { return user_data / "settings.txt"; }

}  // namespace bd::ui
