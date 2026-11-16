// The settings book: one chapter of settings to a page, with the chapters
// listed beside it. Made entirely from the settings' descriptions
// (ui/game_settings.h), so a new setting needs no work here.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "app/menu_widgets.h"
#include "platform/settings.h"

namespace bd::app {

class SettingsBook {
 public:
  // Draws the book in the space left in the current ImGui window. Returns
  // the keys the player changed this frame; they are already set.
  std::vector<std::string> draw(platform::Settings& settings);
  // Drawn in the look of the game's settings menu from now on (each setting
  // on a strip of parchment, its help in the message bar), or ImGui's own
  // with null. `look` must outlive its use here.
  void set_look(const menu::Look* look) { look_ = look; }
  // In the game's look: the help of the setting under the pointer, for the
  // message bar; empty when there is none.
  const std::string& hovered_help() const { return hovered_help_; }

 private:
  void draw_setting(platform::Settings& settings, const platform::SettingDef& def, std::vector<std::string>& changed);
  void draw_setting_in_look(platform::Settings& settings, const platform::SettingDef& def,
                            std::vector<std::string>& changed);
  void draw_in_look(platform::Settings& settings, const std::vector<std::string>& chapters,
                    std::vector<std::string>& changed);

  const menu::Look* look_ = nullptr;
  std::string hovered_help_;

  std::size_t chapter_ = 0;
  // The chapter was chosen in the index: the tabs, when shown, follow.
  bool chapter_changed_ = true;
  // A value being edited (a slider being dragged, text being typed): set
  // once the editing ends.
  std::map<std::string, int> numbers_;
  std::map<std::string, std::string> texts_;
};

}  // namespace bd::app
