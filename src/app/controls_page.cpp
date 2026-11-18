#include "app/controls_page.h"

#include <algorithm>

#include "imgui.h"
#include "ui/game_settings.h"

namespace bd::app {

ControlsPage::~ControlsPage() {
  for (SDL_Gamepad* pad : opened_) SDL_CloseGamepad(pad);
}

std::string ControlsPage::newly_pressed(bool keyboard) {
  std::string pressed;
  if (keyboard) {
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    keys_held_.resize(static_cast<std::size_t>(std::max(count, 0)), false);
    for (int code = 0; code < count; ++code) {
      const bool held = state[code];
      if (held && !keys_held_[static_cast<std::size_t>(code)] && pressed.empty()) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
        if (name != nullptr && *name != '\0') pressed = name;
      }
      keys_held_[static_cast<std::size_t>(code)] = held;
    }
    return pressed;
  }
  // Every controller, opened here if nothing else has it open.
  int count = 0;
  std::vector<SDL_Gamepad*> pads;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
      if (pad == nullptr && (pad = SDL_OpenGamepad(ids[i])) != nullptr) opened_.push_back(pad);
      if (pad != nullptr) pads.push_back(pad);
    }
    SDL_free(ids);
  }
  buttons_held_.resize(pads.size() * SDL_GAMEPAD_BUTTON_COUNT, false);
  for (std::size_t p = 0; p < pads.size(); ++p) {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
      const auto button = static_cast<SDL_GamepadButton>(b);
      const bool held = SDL_GetGamepadButton(pads[p], button);
      const std::size_t at = p * SDL_GAMEPAD_BUTTON_COUNT + static_cast<std::size_t>(b);
      // The guide button opens the game's menu: not bound.
      if (held && !buttons_held_[at] && pressed.empty() && button != SDL_GAMEPAD_BUTTON_GUIDE) {
        const char* name = SDL_GetGamepadStringForButton(button);
        if (name != nullptr && *name != '\0') pressed = name;
      }
      buttons_held_[at] = held;
    }
  }
  return pressed;
}

bool ControlsPage::anything_held() {
  newly_pressed(true);
  newly_pressed(false);
  return std::find(keys_held_.begin(), keys_held_.end(), true) != keys_held_.end() ||
         std::find(buttons_held_.begin(), buttons_held_.end(), true) != buttons_held_.end();
}

void ControlsPage::stop_waiting() { waiting_for_.clear(); }

void ControlsPage::draw(platform::Settings& settings, const menu::Look* look, std::vector<std::string>& changed) {
  hovered_help_.clear();

  // A key or button pressed while waiting is bound; Escape stops waiting.
  if (waiting()) {
    const std::string key = newly_pressed(true);
    const std::string button = waiting_keyboard_ ? std::string() : newly_pressed(false);
    const std::string pressed = waiting_keyboard_ ? key : button;
    if (key == "Escape") {
      stop_waiting();
      settling_ = true;
    } else if (!pressed.empty()) {
      if (settings.set(waiting_for_, pressed).ok()) changed.push_back(waiting_for_);
      stop_waiting();
      settling_ = true;
    }
  } else if (settling_ && !anything_held()) {
    settling_ = false;
    for (SDL_Gamepad* pad : opened_) SDL_CloseGamepad(pad);
    opened_.clear();
    buttons_held_.clear();
  }

  const float em = ImGui::GetFontSize();
  const auto& controls = ui::controls();
  const std::vector<std::string> twice_keys = ui::bound_twice(settings, true);
  const std::vector<std::string> twice_buttons = ui::bound_twice(settings, false);
  const auto twice = [&](const std::string& key) {
    return std::find(twice_keys.begin(), twice_keys.end(), key) != twice_keys.end() ||
           std::find(twice_buttons.begin(), twice_buttons.end(), key) != twice_buttons.end();
  };
  // The other DS buttons a binding's control is also bound to.
  const auto also_on = [&](const ui::Control& mine, bool keyboard) {
    std::string names;
    const std::string value = settings.value(keyboard ? mine.keyboard_key : mine.controller_key);
    for (const ui::Control& other : controls) {
      if (&other == &mine) continue;
      std::string theirs = settings.value(keyboard ? other.keyboard_key : other.controller_key);
      std::string a = value;
      for (std::string* s : {&a, &theirs})
        std::transform(s->begin(), s->end(), s->begin(),
                       [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; });
      if (a == theirs) names += (names.empty() ? "" : ", ") + other.label;
    }
    return names;
  };

  if (look == nullptr) {
    ImGui::TextWrapped("Click a binding, then press the key or controller button for it. Escape stops waiting.");
  }
  if (menu::action_button(look, "Reset to defaults") && !settling_) {
    stop_waiting();
    ui::reset_controls(settings);
    for (const ui::Control& c : controls) {
      changed.push_back(c.keyboard_key);
      changed.push_back(c.controller_key);
    }
  }
  ImGui::Spacing();

  const float label_width = em * 5;
  const float column = std::max(em * 8, (ImGui::GetContentRegionAvail().x - label_width) / 2 - em);
  const auto heading = [&](const char* text) {
    if (look != nullptr) menu::label(look, text);
    else ImGui::TextDisabled("%s", text);
  };
  ImGui::Dummy(ImVec2(label_width, 0));
  ImGui::SameLine(label_width);
  heading("Keyboard");
  ImGui::SameLine(label_width + column + em);
  heading("Controller");

  for (const ui::Control& c : controls) {
    ImGui::PushID(c.keyboard_key.c_str());
    menu::begin_strip(look);
    ImGui::AlignTextToFramePadding();
    if (look != nullptr) menu::label(look, c.label.c_str());
    else ImGui::TextUnformatted(c.label.c_str());
    std::string warning;
    for (const bool keyboard : {true, false}) {
      const std::string& key = keyboard ? c.keyboard_key : c.controller_key;
      ImGui::SameLine(keyboard ? label_width : label_width + column + em);
      const std::string value = settings.value(key);
      std::string text = waiting_for_ == key ? (keyboard ? "Press a key..." : "Press a controller button...")
                         : value.empty()     ? "(none)"
                                             : value;
      text += "##" + key;
      const bool clicked = look != nullptr ? menu::choice_button(look, text.c_str(), waiting_for_ == key, ImVec2(column, 0))
                                           : ImGui::Button(text.c_str(), ImVec2(column, 0));
      if (clicked && !settling_) {
        if (waiting_for_ == key) {
          stop_waiting();
        } else {
          waiting_for_ = key;
          waiting_keyboard_ = keyboard;
          // What is held now is not a press.
          newly_pressed(true);
          newly_pressed(false);
        }
      }
      if (twice(key))
        warning += std::string(warning.empty() ? "" : " ") + (keyboard ? "Key" : "Controller button") +
                   " also bound to " + also_on(c, keyboard) + ".";
    }
    if (!warning.empty()) {
      // Dark red on parchment, amber on ImGui's dark background.
      ImGui::PushStyleColor(ImGuiCol_Text, look != nullptr ? ImVec4(0.6f, 0.08f, 0.04f, 1.0f) : ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
      ImGui::TextWrapped("%s", warning.c_str());
      ImGui::PopStyleColor();
    }
    if (menu::end_strip(look))
      hovered_help_ = "Click a binding, then press the key or controller button for " + c.label +
                      ". Escape stops waiting.";
    ImGui::PopID();
  }
}

}  // namespace bd::app
