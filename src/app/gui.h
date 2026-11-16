// The window program's own screens are drawn with Dear ImGui over SDL's
// renderer. This sets it up for the window, keeps its size in step with the
// display's scale, and gives it Bright Dawn's look.
#pragma once

#include <SDL3/SDL.h>

#include <string>

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

 private:
  void apply_style();

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool started_ = false;
  float scale_ = 0;
};

}  // namespace bd::app
