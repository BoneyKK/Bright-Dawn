#include "app/settings_book.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "ui/game_settings.h"

namespace bd::app {

using platform::SettingDef;
using platform::SettingKind;

std::vector<std::string> SettingsBook::chapters_of(const platform::Settings& settings) {
  std::vector<std::string> chapters = settings.chapters();
  const auto game = std::find(chapters.begin(), chapters.end(), "Game");
  chapters.insert(game != chapters.end() ? game + 1 : chapters.end(), std::string(ui::kControlsChapter));
  return chapters;
}

std::vector<std::string> SettingsBook::draw(platform::Settings& settings) {
  std::vector<std::string> changed;
  const std::vector<std::string> chapters = chapters_of(settings);
  if (chapters.empty()) return changed;
  chapter_ = std::min(chapter_, chapters.size() - 1);
  hovered_help_.clear();
  if (look_ != nullptr) {
    draw_in_look(settings, chapters, changed);
    return changed;
  }

  // The chapters beside the page, or above it where there is not room.
  const float index_width = ImGui::GetFontSize() * 11;
  if (ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 36) {
    ImGui::BeginChild("chapters", ImVec2(index_width, 0), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < chapters.size(); ++i) {
      if (ImGui::Selectable(chapters[i].c_str(), i == chapter_)) {
        chapter_ = i;
        chapter_changed_ = true;
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
  } else if (ImGui::BeginTabBar("chapters", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (std::size_t i = 0; i < chapters.size(); ++i) {
      // Selected only when asked, so that the tab bar follows chapter_.
      const ImGuiTabItemFlags flags = i == chapter_ && chapter_changed_ ? ImGuiTabItemFlags_SetSelected : 0;
      if (ImGui::BeginTabItem(chapters[i].c_str(), nullptr, flags)) {
        if (!chapter_changed_) chapter_ = i;
        ImGui::EndTabItem();
      }
    }
    chapter_changed_ = false;
    ImGui::EndTabBar();
  }

  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted(chapters[chapter_].c_str());
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::Separator();
  if (chapters[chapter_] == ui::kControlsChapter) {
    controls_.draw(settings, nullptr, changed);
  } else {
    for (const SettingDef& def : settings.definitions())
      if (def.chapter == chapters[chapter_]) draw_setting(settings, def, changed);
  }
  ImGui::EndChild();
  return changed;
}

void SettingsBook::draw_setting(platform::Settings& settings, const SettingDef& def,
                                std::vector<std::string>& changed) {
  ImGui::PushID(def.key.c_str());
  ImGui::Spacing();
  const auto set = [&](const std::string& value) {
    if (settings.set(def.key, value).ok()) changed.push_back(def.key);
  };
  switch (def.kind) {
    case SettingKind::Toggle: {
      bool on = settings.toggle(def.key);
      if (ImGui::Checkbox(def.label.c_str(), &on)) set(on ? "on" : "off");
      break;
    }
    case SettingKind::Number: {
      ImGui::TextUnformatted(def.label.c_str());
      const auto editing = numbers_.find(def.key);
      int value = editing != numbers_.end() ? editing->second : static_cast<int>(settings.number(def.key));
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
      ImGui::SliderInt("##value", &value, static_cast<int>(def.low), static_cast<int>(def.high), "%d",
                       ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemActive()) {
        numbers_[def.key] = value;
      } else {
        numbers_.erase(def.key);
        if (value != settings.number(def.key)) set(std::to_string(value));
      }
      break;
    }
    case SettingKind::Choice: {
      ImGui::TextUnformatted(def.label.c_str());
      const std::string& current = settings.choice(def.key);
      for (const platform::SettingChoice& choice : def.choices) {
        ImGui::Indent();
        if (ImGui::RadioButton(choice.label.c_str(), current == choice.value) && current != choice.value)
          set(choice.value);
        ImGui::Unindent();
      }
      break;
    }
    case SettingKind::Text: {
      ImGui::TextUnformatted(def.label.c_str());
      auto editing = texts_.find(def.key);
      if (editing == texts_.end()) editing = texts_.emplace(def.key, settings.value(def.key)).first;
      char buffer[1024];
      std::snprintf(buffer, sizeof buffer, "%s", editing->second.c_str());
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputText("##value", buffer, sizeof buffer)) editing->second = buffer;
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (editing->second != settings.value(def.key)) set(editing->second);
        texts_.erase(def.key);
      } else if (!ImGui::IsItemActive()) {
        texts_.erase(def.key);
      }
      break;
    }
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::Indent();
  ImGui::TextWrapped("%s", def.help.c_str());
  if (settings.overridden(def.key))
    ImGui::TextWrapped("For this run, the command line set it to %s.", settings.value(def.key).c_str());
  ImGui::Unindent();
  ImGui::PopStyleColor();
  if (!settings.is_default(def.key)) {
    ImGui::Indent();
    if (ImGui::SmallButton("Back to the default")) {
      settings.reset(def.key);
      changed.push_back(def.key);
    }
    ImGui::Unindent();
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::PopID();
}

// The game's settings menu: the chapters as buttons on the wooden board,
// and each setting on a strip of parchment, its name in light outlined
// letters and its choices as buttons, the one taken yellow.
void SettingsBook::draw_in_look(platform::Settings& settings, const std::vector<std::string>& chapters,
                                std::vector<std::string>& changed) {
  const float em = ImGui::GetFontSize();
  const bool beside = ImGui::GetContentRegionAvail().x >= em * 34;
  if (beside) ImGui::BeginChild("chapters", ImVec2(em * 9, 0));
  for (std::size_t i = 0; i < chapters.size(); ++i) {
    const ImVec2 size(beside ? -1.0f : 0.0f, 0.0f);
    if (!beside && i != 0) {
      // Side by side while there is room.
      const float width = ImGui::CalcTextSize(chapters[i].c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
      ImGui::SameLine();
      if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
    }
    if (menu::choice_button(look_, chapters[i].c_str(), i == chapter_, size)) chapter_ = i;
  }
  if (beside) {
    ImGui::EndChild();
    ImGui::SameLine();
  }
  ImGui::BeginChild("page", ImVec2(0, 0));
  if (chapters[chapter_] == ui::kControlsChapter) {
    controls_.draw(settings, look_, changed);
    hovered_help_ = controls_.hovered_help();
  } else {
    for (const SettingDef& def : settings.definitions())
      if (def.chapter == chapters[chapter_]) draw_setting_in_look(settings, def, changed);
  }
  ImGui::EndChild();
}

void SettingsBook::draw_setting_in_look(platform::Settings& settings, const SettingDef& def,
                                        std::vector<std::string>& changed) {
  ImGui::PushID(def.key.c_str());
  const auto set = [&](const std::string& value) {
    if (settings.set(def.key, value).ok()) changed.push_back(def.key);
  };
  // Buttons side by side while there is room, then on the next line.
  bool first = true;
  const auto button = [&](const char* text, bool chosen) {
    const float width = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2;
    if (!first) {
      ImGui::SameLine();
      if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
    }
    first = false;
    return menu::choice_button(look_, text, chosen);
  };
  menu::begin_strip(look_);
  // As the game's settings menu has them: the label on the left of the
  // strip, what can be chosen on the right. Where there is not room, the
  // label above.
  const float em = ImGui::GetFontSize();
  const float room = ImGui::GetContentRegionAvail().x;
  const bool beside = room >= em * 24;
  if (beside) {
    const float label_width = std::min(room * 0.42f, em * 13);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_width - em * 0.5f);
    menu::label(look_, def.label.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(label_width);
    ImGui::BeginGroup();
  } else {
    menu::label(look_, def.label.c_str());
  }
  switch (def.kind) {
    case SettingKind::Toggle: {
      const bool on = settings.toggle(def.key);
      if (button("On", on) && !on) set("on");
      if (button("Off", !on) && on) set("off");
      break;
    }
    case SettingKind::Number: {
      const auto editing = numbers_.find(def.key);
      int value = editing != numbers_.end() ? editing->second : static_cast<int>(settings.number(def.key));
      ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16));
      ImGui::SliderInt("##value", &value, static_cast<int>(def.low), static_cast<int>(def.high), "%d",
                       ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemActive()) {
        numbers_[def.key] = value;
      } else {
        numbers_.erase(def.key);
        if (value != settings.number(def.key)) set(std::to_string(value));
      }
      break;
    }
    case SettingKind::Choice: {
      const std::string& current = settings.choice(def.key);
      for (const platform::SettingChoice& choice : def.choices)
        if (button(choice.label.c_str(), current == choice.value) && current != choice.value) set(choice.value);
      break;
    }
    case SettingKind::Text: {
      auto editing = texts_.find(def.key);
      if (editing == texts_.end()) editing = texts_.emplace(def.key, settings.value(def.key)).first;
      char buffer[1024];
      std::snprintf(buffer, sizeof buffer, "%s", editing->second.c_str());
      ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 0.6f);
      if (ImGui::InputText("##value", buffer, sizeof buffer)) editing->second = buffer;
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (editing->second != settings.value(def.key)) set(editing->second);
        texts_.erase(def.key);
      } else if (!ImGui::IsItemActive()) {
        texts_.erase(def.key);
      }
      break;
    }
  }
  if (!settings.is_default(def.key)) {
    const char* text = "Back to the default";
    const float width = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2;
    if (def.kind != SettingKind::Text) {
      ImGui::SameLine();
      if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
    }
    if (menu::action_button(look_, text)) {
      settings.reset(def.key);
      changed.push_back(def.key);
    }
  }
  if (beside) ImGui::EndGroup();
  if (menu::end_strip(look_)) {
    hovered_help_ = def.help;
    if (settings.overridden(def.key))
      hovered_help_ += " For this run, the command line set it to " + settings.value(def.key) + ".";
  }
  ImGui::PopID();
}

}  // namespace bd::app
