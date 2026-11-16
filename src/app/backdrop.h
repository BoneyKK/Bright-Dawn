// A picture behind the program's own screens, filling the window: the book
// of the game's main menu, read from the player's ROM (nothing of the game
// comes with Bright Dawn). Scaled as the game's screens are by default,
// sharp pixels with smooth edges: enlarged to a whole multiple of its size
// once, here, then drawn to the window with linear filtering.
#pragma once

#include <SDL3/SDL.h>

#include <optional>

#include "assets/screen_picture.h"

namespace bd::app {

class Backdrop {
 public:
  Backdrop() = default;
  Backdrop(const Backdrop&) = delete;
  Backdrop& operator=(const Backdrop&) = delete;
  ~Backdrop() { clear(); }

  void set(assets::Picture picture);
  void clear();
  bool shown() const { return picture_.has_value(); }
  // Covers the renderer's whole output, cut at two sides when the window's
  // shape is not the picture's. Does nothing without a picture.
  void draw(SDL_Renderer* renderer);
  // Before the renderer goes.
  void release();

 private:
  bool prepare(SDL_Renderer* renderer, int factor);

  std::optional<assets::Picture> picture_;
  SDL_Texture* texture_ = nullptr;
  int factor_ = 0;
  bool failed_ = false;
};

}  // namespace bd::app
