// Settings: values checked against their descriptions, the file, one-run
// overrides; Bright Dawn's own settings; following the game builder.
#include <algorithm>
#include <string>

#include "platform/settings.h"
#include "temp_dir.h"
#include "test.h"
#include "ui/build_progress.h"
#include "ui/game_settings.h"

namespace {

using bd::platform::SettingDef;
using bd::platform::SettingKind;
using bd::platform::Settings;
using bd_test::TempDir;

std::vector<SettingDef> sample() {
  SettingDef scale;
  scale.key = "display.scale";
  scale.chapter = "Display";
  scale.label = "Window size";
  scale.help = "How big.";
  scale.kind = SettingKind::Number;
  scale.low = 1;
  scale.high = 8;
  scale.default_value = "2";
  SettingDef full;
  full.key = "display.fullscreen";
  full.chapter = "Display";
  full.label = "Full screen";
  full.help = "Fills it.";
  full.kind = SettingKind::Toggle;
  full.default_value = "off";
  SettingDef filter;
  filter.key = "graphics.textures";
  filter.chapter = "Graphics";
  filter.label = "Textures";
  filter.help = "Which.";
  filter.kind = SettingKind::Choice;
  filter.choices = {{"sharp", "Sharp"}, {"smooth", "Smooth"}};
  filter.default_value = "sharp";
  SettingDef rom;
  rom.key = "game.rom";
  rom.label = "ROM";
  rom.help = "Where.";
  rom.kind = SettingKind::Text;
  return {scale, full, filter, rom};
}

}  // namespace

TEST(settings_start_at_their_defaults) {
  Settings s(sample());
  CHECK(s.number("display.scale") == 2 && !s.toggle("display.fullscreen") && s.choice("graphics.textures") == "sharp");
  CHECK(s.value("game.rom").empty() && s.is_default("game.rom"));
  CHECK(s.value("no.such").empty() && s.find("no.such") == nullptr);
  // Hidden settings (no chapter) are in no chapter.
  CHECK((s.chapters() == std::vector<std::string>{"Display", "Graphics"}));
}

TEST(settings_take_only_valid_values) {
  Settings s(sample());
  const auto revision = s.revision();
  CHECK(s.set("display.scale", " 5 ").ok() && s.number("display.scale") == 5);
  CHECK(s.revision() == revision + 1);
  CHECK(!s.set("display.scale", "9").ok() && !s.set("display.scale", "two").ok() && !s.set("display.scale", "").ok());
  CHECK(s.number("display.scale") == 5);
  CHECK(s.set("display.fullscreen", "Yes").ok() && s.toggle("display.fullscreen"));
  CHECK(s.set("display.fullscreen", "0").ok() && !s.toggle("display.fullscreen"));
  CHECK(!s.set("display.fullscreen", "maybe").ok());
  CHECK(s.set("graphics.textures", "SMOOTH").ok() && s.choice("graphics.textures") == "smooth");
  CHECK(!s.set("graphics.textures", "blurry").ok());
  CHECK(!s.set("game.rom", "two\nlines").ok());
  CHECK(!s.set("no.such", "1").ok());
  // The message names the setting and what it takes.
  const bd::Status bad = s.set("display.scale", "12");
  CHECK(!bad.ok() && bad.error().message == "Window size: \"12\" is not 1 to 8");
  // Setting the value it has already is no change.
  const auto before = s.revision();
  CHECK(s.set("display.scale", "5").ok() && s.revision() == before);
  s.reset("display.scale");
  CHECK(s.number("display.scale") == 2 && s.is_default("display.scale") && s.revision() == before + 1);
}

TEST(settings_can_be_overridden_for_one_run) {
  TempDir dir;
  Settings s(sample());
  CHECK(s.set("display.scale", "3").ok());
  CHECK(s.set_for_run("display.scale", "6").ok() && s.number("display.scale") == 6 && s.overridden("display.scale"));
  CHECK(!s.set_for_run("display.scale", "60").ok() && s.number("display.scale") == 6);
  // What is saved is what was set, not the override.
  CHECK(s.save(dir.path() / "settings.txt").ok());
  Settings t(sample());
  REQUIRE(t.load(dir.path() / "settings.txt").ok());
  CHECK(t.number("display.scale") == 3);
  // Setting the value ends the override.
  CHECK(s.set("display.scale", "4").ok() && !s.overridden("display.scale") && s.number("display.scale") == 4);
}

TEST(settings_file_round_trip) {
  TempDir dir;
  Settings s(sample());
  CHECK(s.set("display.scale", "4").ok() && s.set("graphics.textures", "smooth").ok());
  CHECK(s.set("game.rom", "C:\\Games\\Golden Sun = Dark Dawn.nds").ok());
  const std::filesystem::path file = dir.path() / "sub" / "settings.txt";
  REQUIRE(s.save(file).ok());
  CHECK(!std::filesystem::exists(dir.path() / "sub" / "settings.txt.new"));
  const std::string text = dir.read("sub/settings.txt");
  // Every setting, described.
  CHECK(text.find("# Display, Window size: How big.\n# 1 to 8. Default: 2.\ndisplay.scale = 4\n") != std::string::npos);
  CHECK(text.find("# On or off. Default: off.\ndisplay.fullscreen = off\n") != std::string::npos);
  CHECK(text.find("# One of sharp, smooth. Default: sharp.\ngraphics.textures = smooth\n") != std::string::npos);
  Settings t(sample());
  const auto problems = t.load(file);
  REQUIRE(problems.ok());
  CHECK(problems.value().empty());
  CHECK(t.number("display.scale") == 4 && t.choice("graphics.textures") == "smooth");
  // The first "=" separates the key; the rest is the value.
  CHECK(t.value("game.rom") == "C:\\Games\\Golden Sun = Dark Dawn.nds");
  CHECK(t.to_text() == text);
  // No file: the defaults, and no problem.
  Settings u(sample());
  const auto none = u.load(dir.path() / "none.txt");
  CHECK(none.ok() && none.value().empty() && u.number("display.scale") == 2);
}

TEST(settings_file_problems_keep_the_default_and_unknown_keys_are_kept) {
  Settings s(sample());
  const auto problems = s.from_text(
      "\xEF\xBB\xBF# a comment\r\n"
      "display.scale = 40\r\n"
      "Display.Fullscreen = on\n"
      "just words\n"
      "future.setting = something new\n"
      "\n"
      "graphics.textures = smooth");
  REQUIRE(problems.size() == 2);
  CHECK(problems[0] == "line 2: Window size: \"40\" is not 1 to 8; the default, 2, is kept");
  CHECK(problems[1] == "line 4: not \"key = value\": just words");
  CHECK(s.number("display.scale") == 2 && s.toggle("display.fullscreen") && s.choice("graphics.textures") == "smooth");
  const std::string text = s.to_text();
  CHECK(text.find("future.setting = something new\n") != std::string::npos);
  CHECK(text.find("just words") == std::string::npos);
  // Read back, the unknown line is there once.
  Settings t(sample());
  CHECK(t.from_text(text).empty());
  CHECK(t.to_text() == text);
}

TEST(game_settings_are_valid_and_shown_in_chapters) {
  // The constructor checks the keys and the defaults (in builds with checks).
  Settings s(bd::ui::game_setting_definitions());
  for (const SettingDef& def : s.definitions()) {
    CHECK(Settings::normalised(def, def.default_value) == def.default_value);
    CHECK(!def.label.empty() && !def.help.empty());
  }
  CHECK((s.chapters() == std::vector<std::string>{"Display", "Graphics", "Game", "Builder", "Log"}));
  // Sharp pixels with smooth edges, the look of melonDS's display, by
  // default; 3D textures as on the DS.
  CHECK(bd::ui::screen_filter(s) == bd::gfx::ScreenFilter::SharpSmoothEdges && !bd::ui::smooth_textures(s));
  CHECK(s.set(bd::ui::key::kScreenFilter, "sharp").ok() && bd::ui::screen_filter(s) == bd::gfx::ScreenFilter::Sharp);
  CHECK(s.set(bd::ui::key::kScreenFilter, "smooth").ok() && bd::ui::screen_filter(s) == bd::gfx::ScreenFilter::Smooth);
  CHECK(s.set(bd::ui::key::kTextures, "smooth").ok() && bd::ui::smooth_textures(s));
  // The console's language: English unless chosen, each choice the
  // firmware's own value for it.
  using Language = bd::nds::UserSettings::Language;
  CHECK(bd::ui::game_language(s) == Language::English);
  const std::pair<const char*, Language> languages[] = {{"french", Language::French},   {"german", Language::German},
                                                        {"italian", Language::Italian}, {"spanish", Language::Spanish},
                                                        {"japanese", Language::Japanese}, {"english", Language::English}};
  for (const auto& [name, language] : languages) CHECK(s.set(bd::ui::key::kLanguage, name).ok() && bd::ui::game_language(s) == language);
  CHECK(!s.set(bd::ui::key::kLanguage, "klingon").ok() && bd::ui::game_language(s) == Language::English);
  const bd::gfx::LayoutOptions defaults = bd::ui::layout_options(s);
  CHECK(defaults.arrangement == bd::gfx::Arrangement::Stacked && defaults.scaling == bd::gfx::Scaling::WholeNumbers);
  CHECK(defaults.gap == 0 && !defaults.bottom_first);
  CHECK(s.set(bd::ui::key::kArrangement, "side_by_side").ok() && s.set(bd::ui::key::kScaling, "fill").ok());
  CHECK(s.set(bd::ui::key::kGap, "12").ok() && s.set(bd::ui::key::kTouchScreenFirst, "on").ok());
  const bd::gfx::LayoutOptions changed = bd::ui::layout_options(s);
  CHECK(changed.arrangement == bd::gfx::Arrangement::SideBySide && changed.scaling == bd::gfx::Scaling::Fit);
  CHECK(changed.gap == 12 && changed.bottom_first);
}

TEST(control_bindings_default_to_input_defaults) {
  Settings s(bd::ui::game_setting_definitions());
  const auto same = [](std::vector<bd::input::Binding> a, std::vector<bd::input::Binding> b) {
    const auto order = [](const bd::input::Binding& x, const bd::input::Binding& y) { return x.button < y.button; };
    std::sort(a.begin(), a.end(), order);
    std::sort(b.begin(), b.end(), order);
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (a[i].control != b[i].control || a[i].button != b[i].button) return false;
    return true;
  };
  CHECK(same(bd::ui::keyboard_bindings(s), bd::input::default_keyboard_bindings()));
  CHECK(same(bd::ui::controller_bindings(s), bd::input::default_controller_bindings()));
  // Every DS button has a row, and no control is bound twice.
  CHECK(bd::ui::controls().size() == bd::input::kDsButtonCount + 1);  // And the Bottom screen only hotkey.
  CHECK(bd::ui::bound_twice(s, true).empty() && bd::ui::bound_twice(s, false).empty());
  // The bindings are not settings of a chapter: the Controls page draws them.
  CHECK(s.find("controls.keyboard.a") != nullptr && s.find("controls.keyboard.a")->chapter.empty());
  CHECK(s.value("controls.keyboard.select") == "Right Shift" && s.value("controls.controller.up") == "dpup");
}

TEST(control_bindings_are_read_and_saved_in_the_settings_file) {
  Settings s(bd::ui::game_setting_definitions());
  // Read: a key given, a controller button taken away (empty: none).
  CHECK(s.from_text("controls.keyboard.a = Space\ncontrols.controller.b =\n").empty());
  bool a_on_space = false, b_on_controller = false;
  for (const bd::input::Binding& b : bd::ui::keyboard_bindings(s))
    a_on_space = a_on_space || (b.button == bd::input::DsButton::A && b.control == "Space");
  for (const bd::input::Binding& b : bd::ui::controller_bindings(s))
    b_on_controller = b_on_controller || b.button == bd::input::DsButton::B;
  CHECK(a_on_space && !b_on_controller);
  CHECK(bd::ui::controller_bindings(s).size() == bd::input::kDsButtonCount - 1);
  // Saved and read back the same.
  CHECK(s.set("controls.keyboard.start", "Right Shift").ok());
  Settings t(bd::ui::game_setting_definitions());
  CHECK(t.from_text(s.to_text()).empty());
  CHECK(t.value("controls.keyboard.a") == "Space" && t.value("controls.controller.b").empty());
  CHECK(t.value("controls.keyboard.start") == "Right Shift");
  // One key on two DS buttons is found, in any letter case; both are named.
  CHECK((bd::ui::bound_twice(t, true) ==
         std::vector<std::string>{"controls.keyboard.start", "controls.keyboard.select"}));
  CHECK(t.set("controls.keyboard.start", "right shift").ok() && bd::ui::bound_twice(t, true).size() == 2);
  CHECK(bd::ui::bound_twice(t, false).empty());
  // Back to the defaults.
  bd::ui::reset_controls(t);
  CHECK(t.value("controls.keyboard.a") == "X" && t.value("controls.controller.b") == "a");
  CHECK(bd::ui::bound_twice(t, true).empty());
}

TEST(bottom_screen_only_is_an_arrangement_and_a_hotkey) {
  Settings s(bd::ui::game_setting_definitions());
  using bd::ui::Hotkey;
  CHECK(bd::ui::hotkey_binding(s, Hotkey::BottomScreenOnly, true) == "F9");
  CHECK(bd::ui::hotkey_binding(s, Hotkey::BottomScreenOnly, false) == "rightstick");
  CHECK(bd::ui::hotkey_binding(s, Hotkey::None, true).empty());
  // A hotkey is not a DS button's binding.
  for (const bd::input::Binding& b : bd::ui::keyboard_bindings(s)) CHECK(b.control != "F9");
  // From side by side to the touch screen alone, and back.
  CHECK(s.set(bd::ui::key::kArrangement, "side_by_side").ok());
  const auto apply = [&] {
    for (const auto& [key, value] : bd::ui::bottom_screen_switch(s)) CHECK(s.set(key, value).ok());
  };
  apply();
  CHECK(s.choice(bd::ui::key::kArrangement) == "bottom_only");
  CHECK(bd::ui::layout_options(s).arrangement == bd::gfx::Arrangement::BottomOnly);
  apply();
  CHECK(s.choice(bd::ui::key::kArrangement) == "side_by_side");
  // Chosen in the settings instead: back to what was last before.
  CHECK(s.set(bd::ui::key::kArrangement, "stacked").ok());
  apply();
  apply();
  CHECK(s.choice(bd::ui::key::kArrangement) == "stacked");
  // Bound to a DS button's key too: shown as bound twice.
  CHECK(s.set("controls.keyboard.bottom_screen_only", "x").ok());
  CHECK((bd::ui::bound_twice(s, true) ==
         std::vector<std::string>{"controls.keyboard.a", "controls.keyboard.bottom_screen_only"}));
}

TEST(build_progress_follows_the_builder) {
  bd::ui::BuildProgress p;
  CHECK(p.step() == 0 && p.fraction() == 0.0 && !p.within_step());
  p.take_line("Work directory: /x");
  p.take_line("== 1/5. Building bdtool\r");
  CHECK(p.step() == 1 && p.steps() == 5 && p.step_name() == "Building bdtool" && p.fraction() == 0.0);
  p.take_line("[3/12] Building CXX object CMakeFiles/bd_core.dir/src/core/log.cpp.o");
  REQUIRE(p.within_step().has_value());
  CHECK(*p.within_step() == 0.25 && p.fraction() == 0.05);
  // Make's form.
  p.take_line("[ 50%] Built target bd_core");
  CHECK(*p.within_step() == 0.5);
  // Not progress.
  p.take_line("[warning] something");
  p.take_line("[13/12] impossible");
  CHECK(*p.within_step() == 0.5);
  p.take_line("== 5/5. Building the game");
  CHECK(p.step() == 5 && !p.within_step() && p.fraction() == 0.8);
  p.take_line("== 7/5. Not a step");
  CHECK(p.step() == 5);
  p.take_line("The game: /home/me/game/bright-dawn-game");
  CHECK(p.game() == "/home/me/game/bright-dawn-game" && p.error().empty());
  p.take_line("error: building the game failed (exit status 1)");
  CHECK(p.error() == "building the game failed (exit status 1)");
}

TEST(build_progress_reads_version_numbers) {
  CHECK((bd::ui::version_in("Python 3.12.1") == std::array<int, 3>{3, 12, 1}));
  CHECK((bd::ui::version_in("cmake version 3.28.3\n\nCMake suite maintained") == std::array<int, 3>{3, 28, 3}));
  CHECK((bd::ui::version_in("Python 3.10") == std::array<int, 3>{3, 10, 0}));
  CHECK(!bd::ui::version_in("no version here 3").has_value());
  CHECK(!bd::ui::version_in("").has_value());
}
