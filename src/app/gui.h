// The window program's own screens are drawn with Dear ImGui over SDL's
// renderer. This sets it up for the window, keeps its size in step with the
// display's scale, and gives it Bright Dawn's look.
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include "app/game_font.h"
#include "assets/font.h"

struct ImFont;
struct ImGuiStyle;

namespace bd::app {

class Gui {
 public:
  Gui() = default;
  Gui(const Gui&) = delete;
  Gui& operator=(const Gui&) = delete;
  ~Gui();

  bool init(SDL_Window* window, SDL_Renderer* renderer, std::string& error);
  // Every event goes here while a screen of its own is shown.
  void process_event(const SDL_Event& event);
  void begin_frame();
  // Draws what was made since begin_frame. The caller presents.
  void end_frame();
  // Ends it: before the renderer goes. Also done when it is destroyed.
  void shutdown();
  // The look of the game's menus (app/menu_skin.h): pages that show the
  // pictures drawn behind them, and the menus' colours. Or the program's own.
  void set_game_look(bool game_look);
  // The game's font for all text from now on, or the program's own again
  // with null. Between frames only.
  void set_game_font(std::shared_ptr<const assets::Font> font);
  bool has_game_font() const { return plain_ != nullptr; }
  // The game font outlined, as in the game's menus, and plain; the
  // program's own font when there is no game font.
  ImFont* outlined_font() const;
  ImFont* plain_font() const;
  // The size nearest to `wanted` at which the font draws sharply: with the
  // game font, a whole number of screen pixels for each of its pixels.
  float sharp_size(float wanted) const;

 private:
  void apply_style();
  static void apply_game_look(ImGuiStyle& style);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool started_ = false;
  float scale_ = 0;
  bool game_look_ = false;
  // The game font's two kinds, and ImGui's view of them.
  GameFontSource plain_source_;
  GameFontSource outlined_source_;
  ImFont* plain_ = nullptr;
  ImFont* outlined_ = nullptr;
};

}  // namespace bd::app
