#include "ui/game_settings.h"

#include <algorithm>
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
                        "Sets the window's size, in times the DS's size.", 1, 8, 2));
  defs.push_back(choice(key::kArrangement, "Display", "The two screens",
                        "Sets where the DS's two screens are placed.",
                        {{"stacked", "One above the other"}, {"side_by_side", "Side by side"},
                         {"bottom_only", "Bottom screen only"}},
                        "stacked"));
  // The arrangement to go back to from the touch screen alone (Controls,
  // Bottom screen only).
  SettingDef previous;
  previous.key = std::string(key::kPreviousArrangement);
  previous.label = "Screens before the bottom screen only";
  previous.help = "Sets where the Bottom screen only hotkey goes back to.";
  previous.kind = SettingKind::Choice;
  previous.choices = {{"stacked", "One above the other"}, {"side_by_side", "Side by side"}};
  previous.default_value = "stacked";
  defs.push_back(previous);
  defs.push_back(toggle(key::kTouchScreenFirst, "Display", "Touch screen first",
                        "Puts the touch screen above or left of the other.", false));
  defs.push_back(number(key::kGap, "Display", "Gap between the screens",
                        "Sets the space between the screens, in DS pixels.", 0, 96, 0));
  defs.push_back(choice(key::kScaling, "Display", "Scaling",
                        "Scales the screens by whole numbers, or to fill the window.",
                        {{"whole", "Whole numbers"}, {"fill", "Fill the window"}}, "whole"));
  defs.push_back(choice(key::kScreenFilter, "Display", "Screen filtering",
                        "Sets how the screens' pixels are smoothed when scaled.",
                        {{"sharp_smooth_edges", "Sharp pixels, smooth edges"},
                         {"smooth", "Smooth (bilinear)"},
                         {"sharp", "Sharp (nearest)"}},
                        "sharp_smooth_edges"));
  defs.push_back(toggle(key::kFullscreen, "Display", "Full screen", "Starts full screen; F11 switches it.", false));
  defs.push_back(toggle(key::kVsync, "Display", "Wait for the screen's refresh",
                        "Shows each picture when the screen refreshes, without tearing.", true));
  defs.push_back(choice(key::kRenderer, "Display", "Drawing",
                        "Sets what draws the window, from the next start.",
                        {{"vulkan", "Vulkan"}, {"system", "The system's own"}}, "vulkan"));
  defs.push_back(choice(key::k3DDrawing, "Graphics", "3D drawing",
                        "Sets what draws 3D scenes: the graphics hardware (fast, sharper) or the processor (exact).",
                        {{"hardware", "Graphics hardware"}, {"processor", "Processor"}}, "hardware"));
  defs.push_back(choice(key::k3DResolution, "Graphics", "3D resolution",
                        "Sets how many times the DS's resolution the graphics hardware draws 3D scenes at.",
                        {{"auto", "To fit the window"},
                         {"1", "1x (256 by 192)"},
                         {"2", "2x"},
                         {"3", "3x"},
                         {"4", "4x"},
                         {"5", "5x"},
                         {"6", "6x"},
                         {"7", "7x"},
                         {"8", "8x (2048 by 1536)"}},
                        "auto"));
  defs.push_back(toggle(key::kFineCoordinates, "Graphics", "High-resolution coordinates",
                        "Above 1x, places 3D models between the DS's pixels, so that they move smoothly.", true));
  defs.push_back(choice(key::kEdgeMarking, "Graphics", "Outlines",
                        "Sets the outlines the game draws around 3D models (edge marking): as wide as a DS pixel at "
                        "any resolution, one pixel wide, or none.",
                        {{"original", "Original thickness"}, {"pixel", "1 pixel"}, {"off", "Off"}}, "original"));
  defs.push_back(choice(key::kTextures, "Graphics", "Textures in 3D scenes",
                        "Sets whether the processor draws 3D textures sharp or smoothed.",
                        {{"sharp", "Sharp (nearest)"}, {"smooth", "Smooth (bilinear)"}}, "sharp"));
  defs.push_back(toggle(key::kSound, "Sound", "Sound", "Plays the game's sound and music.", true));
  defs.push_back(number(key::kVolume, "Sound", "Volume", "Sets how loud the game's sound is, in percent.", 0, 100, 100));
  defs.push_back(toggle(key::kPlayBuilt, "Game", "Play the built game",
                        "Plays the game built from your ROM, when there is one.",
                        true));
  defs.push_back(choice(key::kLanguage, "Game", "Language",
                        "Sets the language of the game's text, from the next start of the game.",
                        {{"english", "English"},
                         {"french", "French (Fran\u00e7ais)"},
                         {"german", "German (Deutsch)"},
                         {"italian", "Italian (Italiano)"},
                         {"spanish", "Spanish (Espa\u00f1ol)"},
                         {"japanese", "Japanese"}},
                        "english"));
  SettingDef rom;
  rom.key = std::string(key::kRom);
  rom.label = "ROM";
  rom.help = "Holds the ROM last chosen.";
  rom.kind = SettingKind::Text;
  defs.push_back(rom);
  defs.push_back(number(key::kBuildJobs, "Builder", "Parallel jobs",
                        "Sets how many files the builder compiles at once; 0 for one for each processor.",
                        0, 256, 0));
  SettingDef compiler;
  compiler.key = std::string(key::kBuildCompiler);
  compiler.chapter = "Builder";
  compiler.label = "Compiler";
  compiler.help = "Names a C++ compiler for the builder to use, such as g++ or clang++; empty for Zig.";
  compiler.kind = SettingKind::Text;
  defs.push_back(compiler);
  defs.push_back(choice(key::kLogLevel, "Log", "Detail",
                        "Sets how much the log records.",
                        {{"error", "Errors"}, {"warning", "Warnings"}, {"info", "What happens (normal)"},
                         {"debug", "In detail"}},
                        "info"));
  defs.push_back(number(key::kLogsKept, "Log", "Logs kept", "Sets how many runs' logs are kept.", 1, 100, 10));
  // The bindings: not in a chapter of their own, as the Controls page draws
  // them; their defaults are input's.
  const auto bound_by_default = [](const Control& c, bool keyboard) {
    // Hotkeys: F9, and the right stick's button, which the DS has no use for.
    if (c.hotkey == Hotkey::BottomScreenOnly) return std::string(keyboard ? "F9" : "rightstick");
    for (const input::Binding& b :
         keyboard ? input::default_keyboard_bindings() : input::default_controller_bindings())
      if (b.button == c.button) return b.control;
    return std::string();
  };
  for (const Control& c : controls()) {
    for (const bool keyboard : {true, false}) {
      SettingDef d;
      d.key = keyboard ? c.keyboard_key : c.controller_key;
      d.label = (keyboard ? "Key for " : "Controller button for ") + c.label;
      d.help = "Binds a control to it; empty for none.";
      d.kind = SettingKind::Text;
      d.default_value = bound_by_default(c, keyboard);
      defs.push_back(d);
    }
  }
  return defs;
}

const std::vector<Control>& controls() {
  static const std::vector<Control> kControls = [] {
    using input::DsButton;
    std::vector<Control> list;
    for (const DsButton button : {DsButton::A, DsButton::B, DsButton::X, DsButton::Y, DsButton::L, DsButton::R,
                                  DsButton::Start, DsButton::Select, DsButton::Up, DsButton::Down, DsButton::Left,
                                  DsButton::Right}) {
      std::string id = input::name(button);
      std::transform(id.begin(), id.end(), id.begin(),
                     [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; });
      list.push_back({input::name(button), "controls.keyboard." + id, "controls.controller." + id, button});
    }
    list.push_back({"Bottom screen only", "controls.keyboard.bottom_screen_only",
                    "controls.controller.bottom_screen_only", input::DsButton::A, Hotkey::BottomScreenOnly});
    return list;
  }();
  return kControls;
}

namespace {

std::vector<input::Binding> bindings(const platform::Settings& settings, bool keyboard) {
  std::vector<input::Binding> list;
  for (const Control& c : controls()) {
    if (c.hotkey != Hotkey::None) continue;
    const std::string control = settings.value(keyboard ? c.keyboard_key : c.controller_key);
    if (!control.empty()) list.push_back({control, c.button});
  }
  return list;
}

bool same_name(const std::string& a, const std::string& b) {
  const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
           return lower(x) == lower(y);
         });
}

}  // namespace

std::vector<input::Binding> keyboard_bindings(const platform::Settings& settings) { return bindings(settings, true); }
std::vector<input::Binding> controller_bindings(const platform::Settings& settings) {
  return bindings(settings, false);
}

std::vector<std::string> bound_twice(const platform::Settings& settings, bool keyboard) {
  std::vector<std::string> keys;
  const std::vector<Control>& list = controls();
  const auto key_of = [&](const Control& c) { return keyboard ? c.keyboard_key : c.controller_key; };
  for (std::size_t i = 0; i < list.size(); ++i) {
    const std::string mine = settings.value(key_of(list[i]));
    if (mine.empty()) continue;
    for (std::size_t j = 0; j < list.size(); ++j) {
      if (j != i && same_name(mine, settings.value(key_of(list[j])))) {
        keys.push_back(key_of(list[i]));
        break;
      }
    }
  }
  return keys;
}

std::string hotkey_binding(const platform::Settings& settings, Hotkey hotkey, bool keyboard) {
  for (const Control& c : controls())
    if (c.hotkey == hotkey && hotkey != Hotkey::None) return settings.value(keyboard ? c.keyboard_key : c.controller_key);
  return {};
}

std::vector<std::pair<std::string, std::string>> bottom_screen_switch(const platform::Settings& settings) {
  const std::string current = settings.choice(key::kArrangement);
  if (current == "bottom_only") return {{std::string(key::kArrangement), settings.choice(key::kPreviousArrangement)}};
  return {{std::string(key::kPreviousArrangement), current}, {std::string(key::kArrangement), "bottom_only"}};
}

void reset_controls(platform::Settings& settings) {
  for (const Control& c : controls()) {
    settings.reset(c.keyboard_key);
    settings.reset(c.controller_key);
  }
}

gfx::LayoutOptions layout_options(const platform::Settings& settings) {
  gfx::LayoutOptions options;
  const std::string& arrangement = settings.choice(key::kArrangement);
  options.arrangement = arrangement == "side_by_side"  ? gfx::Arrangement::SideBySide
                        : arrangement == "bottom_only" ? gfx::Arrangement::BottomOnly
                                                       : gfx::Arrangement::Stacked;
  options.scaling = settings.choice(key::kScaling) == "fill" ? gfx::Scaling::Fit : gfx::Scaling::WholeNumbers;
  options.gap = static_cast<int>(settings.number(key::kGap));
  options.bottom_first = settings.toggle(key::kTouchScreenFirst);
  return options;
}

gfx::ScreenFilter screen_filter(const platform::Settings& settings) {
  const std::string& v = settings.choice(key::kScreenFilter);
  if (v == "smooth") return gfx::ScreenFilter::Smooth;
  if (v == "sharp") return gfx::ScreenFilter::Sharp;
  return gfx::ScreenFilter::SharpSmoothEdges;
}

bool smooth_textures(const platform::Settings& settings) { return settings.choice(key::kTextures) == "smooth"; }

bool hardware_3d(const platform::Settings& settings) { return settings.choice(key::k3DDrawing) == "hardware"; }

nds::EdgeMarking edge_marking(const platform::Settings& settings) {
  using Marking = nds::EdgeMarking;
  const std::string chosen = settings.choice(key::kEdgeMarking);
  return chosen == "off" ? Marking::Off : chosen == "pixel" ? Marking::OnePixel : Marking::OriginalThickness;
}

int resolution_3d(const platform::Settings& settings, int shown_height) {
  const std::string chosen = settings.choice(key::k3DResolution);
  if (chosen.size() == 1 && chosen[0] >= '1' && chosen[0] <= '8') return chosen[0] - '0';
  // No more pixels than are shown: the rest of the way is smoothed.
  return std::clamp(shown_height / 192, 1, 8);
}

nds::UserSettings::Language game_language(const platform::Settings& settings) {
  using Language = nds::UserSettings::Language;
  const std::string& v = settings.choice(key::kLanguage);
  if (v == "french") return Language::French;
  if (v == "german") return Language::German;
  if (v == "italian") return Language::Italian;
  if (v == "spanish") return Language::Spanish;
  if (v == "japanese") return Language::Japanese;
  return Language::English;
}

std::filesystem::path settings_file(const std::filesystem::path& user_data) { return user_data / "settings.txt"; }

}  // namespace bd::ui
