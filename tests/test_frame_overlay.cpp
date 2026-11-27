// The frame overlay (app/frame_overlay.h): drawn over the window only, in
// its corner box; nothing outside the box changes, the game's screens are
// not touched, and the renderer's state is as it was. On SDL's software
// renderer, so no graphics hardware is needed.
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app/frame_overlay.h"
#include "test.h"

namespace {

std::uint32_t pixel(SDL_Surface* s, int x, int y) {
  return static_cast<const std::uint32_t*>(s->pixels)[static_cast<std::size_t>(y) * (s->pitch / 4) + x];
}

}  // namespace

TEST(frame_overlay_lines_name_the_three_costliest_columns) {
  const std::vector<std::string> names{"frame", "total_us", "arm9_us", "scene_us", "upload_us", "present_us", "polygons"};
  const std::vector<double> values{41, 16000, 9000, 500, 4000, 2500, 9999};
  const auto lines = bd::app::frame_overlay_lines(42, names, values, "Test GPU", true);
  CHECK(lines.size() == 4);
  CHECK(lines[0] == "frame 42  HELD");
  CHECK(lines[1] == "host 16.00 ms (frame 41)");
  CHECK(lines[2] == "arm9 9.00  upload 4.00  present 2.50");
  CHECK(lines[3] == "GPU: Test GPU");
  CHECK(bd::app::frame_overlay_lines(1, {}, {}, "", false)[1] == "no frame-log row yet");
}

TEST(frame_overlay_changes_only_its_box_and_restores_the_renderer) {
  SDL_Surface* window = SDL_CreateSurface(640, 480, SDL_PIXELFORMAT_XRGB8888);
  CHECK(window != nullptr);
  SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(window);
  CHECK(renderer != nullptr);
  // The game's screen, as a texture drawn into the window.
  std::vector<std::uint32_t> screen(256 * 192);
  for (std::size_t i = 0; i < screen.size(); ++i) screen[i] = 0xFF000000u | static_cast<std::uint32_t>(i * 2654435761u >> 8);
  const std::vector<std::uint32_t> screen_before = screen;
  SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 192);
  CHECK(texture != nullptr);
  SDL_UpdateTexture(texture, nullptr, screen.data(), 256 * 4);
  const SDL_FRect at{0, 0, 512, 384};
  SDL_RenderTexture(renderer, texture, nullptr, &at);
  SDL_RenderPresent(renderer);
  std::vector<std::uint32_t> before(640 * 480);
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 640; ++x) before[static_cast<std::size_t>(y) * 640 + x] = pixel(window, x, y);

  SDL_RenderTexture(renderer, texture, nullptr, &at);  // Again, as the game screen draws each picture.
  // State of its own, to be left as it is.
  SDL_SetRenderDrawColor(renderer, 1, 2, 3, 4);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
  SDL_SetRenderScale(renderer, 1.5f, 1.5f);
  const std::vector<std::string> lines{"frame 1", "host 1.00 ms", "arm9 1.00", "GPU: none"};
  const SDL_FRect box = bd::app::draw_frame_overlay(renderer, lines);
  SDL_RenderPresent(renderer);

  CHECK(box.w > 0 && box.h > 0);
  int changed_inside = 0, changed_outside = 0;
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 640; ++x) {
      const bool inside = x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h;
      if (pixel(window, x, y) != before[static_cast<std::size_t>(y) * 640 + x]) (inside ? changed_inside : changed_outside)++;
    }
  CHECK(changed_outside == 0);
  CHECK(changed_inside > 0);
  CHECK(screen == screen_before);
  float sx = 0, sy = 0;
  SDL_GetRenderScale(renderer, &sx, &sy);
  CHECK(sx == 1.5f && sy == 1.5f);
  Uint8 r = 0, g = 0, b = 0, a = 0;
  SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
  CHECK(r == 1 && g == 2 && b == 3 && a == 4);
  SDL_BlendMode blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(renderer, &blend);
  CHECK(blend == SDL_BLENDMODE_ADD);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(window);
}
