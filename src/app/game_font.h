// The game's font (assets/font.h) as Dear ImGui fonts, read from the
// player's ROM when one is chosen. Each pixel of a glyph becomes a square of
// `scale` by `scale` screen pixels, so the text keeps the DS's crisp look.
// Two kinds: plain, and outlined as the game draws its menus' text (the
// outline is black; the text's colour fills the glyph). Characters the game
// font lacks come from ImGui's own font, merged in.
#pragma once

#include <memory>

#include "assets/font.h"

struct ImFont;
struct ImFontLoader;

namespace bd::app {

// ImGui's interface to one way of drawing the font. What a source passes as
// ImFontConfig::FontData.
struct GameFontSource {
  std::shared_ptr<const assets::Font> font;
  bool outline = false;
};

const ImFontLoader* game_font_loader();

// How many screen pixels a font pixel becomes at ImGui font size `size`.
int game_font_scale(const assets::Font& font, float size);

}  // namespace bd::app
