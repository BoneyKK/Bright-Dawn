// Widgets in the look of the game's menus (app/menu_skin.h), for the
// launcher and the settings book: buttons for a choice, as the settings menu
// shows them (the one taken yellow, the others grey), blue buttons that do
// something, light outlined labels for the wooden board, lines laid on
// strips of parchment, and the blue message bar along the bottom.
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "app/gui.h"
#include "app/menu_skin.h"
#include "imgui.h"

namespace bd::app::menu {

// What drawing in the game's look needs. Without it (null), the widgets
// below fall back to ImGui's own.
struct Look {
  MenuSkin* skin = nullptr;
  SDL_Renderer* renderer = nullptr;
  Gui* gui = nullptr;
};

// A choice: true when clicked.
bool choice_button(const Look* look, const char* label, bool chosen, ImVec2 size = ImVec2(0, 0));
// A button that does something: true when clicked.
bool action_button(const Look* look, const char* label, ImVec2 size = ImVec2(0, 0));
// Light text, outlined in black, as the menus label things on wood.
void label(const Look* look, const char* text);

// A line on a strip of parchment: what is drawn between begin_strip and
// end_strip lies on one. Returns whether the line is hovered.
void begin_strip(const Look* look);
bool end_strip(const Look* look);

// The message bar in `where`, with `text` (wrapped) on it.
void message_bar(const Look* look, ImDrawList* list, const ImRect& where, const std::string& text);

}  // namespace bd::app::menu
