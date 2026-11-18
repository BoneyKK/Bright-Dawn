// The Controls chapter of the settings book: which key and which controller
// button press each of the DS's buttons. A binding's button is clicked, then
// the key or controller button pressed is bound (Escape stops waiting). A
// control bound to two DS buttons is shown. The bindings are settings
// (ui::controls()), so they are kept in the settings file, and the game
// takes them up as they change.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "app/menu_widgets.h"
#include "platform/settings.h"

namespace bd::app {

class ControlsPage {
 public:
  ControlsPage() = default;
  ControlsPage(const ControlsPage&) = delete;
  ControlsPage& operator=(const ControlsPage&) = delete;
  ~ControlsPage();

  // Draws the page; adds the keys of the settings changed to `changed`.
  // `look`: the game's look, or null for ImGui's own.
  void draw(platform::Settings& settings, const menu::Look* look, std::vector<std::string>& changed);
  // Whether a key or controller button is being waited for.
  bool waiting() const { return !waiting_for_.empty(); }
  // The help of what is under the pointer, in the game's look.
  const std::string& hovered_help() const { return hovered_help_; }

 private:
  // The key or controller button newly pressed since the last call, by its
  // SDL name; empty if none. Escape gives "Escape".
  std::string newly_pressed(bool keyboard);
  // Whether any key or controller button is held.
  bool anything_held();
  void stop_waiting();

  std::string waiting_for_;  // The setting's key; empty: not waiting.
  bool waiting_keyboard_ = true;
  // After a binding, the bind buttons do nothing until everything is let
  // go of, so that the key just bound (Return, say) does not press one.
  bool settling_ = false;
  std::vector<bool> keys_held_;
  std::vector<bool> buttons_held_;
  std::vector<SDL_Gamepad*> opened_;  // Opened here to be read; closed after.
  std::string hovered_help_;
};

}  // namespace bd::app
