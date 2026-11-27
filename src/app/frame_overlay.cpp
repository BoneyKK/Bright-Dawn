#include "app/frame_overlay.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace bd::app {

std::vector<std::string> frame_overlay_lines(std::uint64_t frame, const std::vector<std::string>& names,
                                             const std::vector<double>& values, const std::string& gpu_name, bool held) {
  std::vector<std::string> lines;
  char text[160];
  std::snprintf(text, sizeof text, "frame %" PRIu64 "%s", frame, held ? "  HELD" : "");
  lines.emplace_back(text);
  // The row's times: "total_us" and the columns that end in "_us".
  double total = -1;
  std::vector<std::pair<double, std::string>> times;
  for (std::size_t i = 0; i < names.size() && i < values.size(); ++i) {
    const std::string_view n = names[i];
    if (n == "total_us") total = values[i];
    else if (n.size() > 3 && n.substr(n.size() - 3) == "_us") times.emplace_back(values[i], std::string(n.substr(0, n.size() - 3)));
  }
  if (total >= 0 && !values.empty()) {
    std::snprintf(text, sizeof text, "host %.2f ms (frame %" PRIu64 ")", total / 1000.0, static_cast<std::uint64_t>(values[0]));
    lines.emplace_back(text);
    std::sort(times.begin(), times.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (std::size_t i = 0; i < times.size() && i < 3; ++i) {
      std::snprintf(text, sizeof text, "%s%s %.2f", i == 0 ? "" : "  ", times[i].second.c_str(), times[i].first / 1000.0);
      top += text;
    }
    lines.push_back(top);
  } else {
    lines.emplace_back("no frame-log row yet");
  }
  lines.push_back(gpu_name.empty() ? "GPU: none" : "GPU: " + gpu_name);
  return lines;
}

SDL_FRect draw_frame_overlay(SDL_Renderer* renderer, const std::vector<std::string>& lines) {
  // What it changes, kept to be put back.
  float scale_x = 1, scale_y = 1;
  SDL_GetRenderScale(renderer, &scale_x, &scale_y);
  Uint8 r = 0, g = 0, b = 0, a = 0;
  SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
  SDL_BlendMode blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(renderer, &blend);
  SDL_Rect clip{};
  const bool clipped = SDL_RenderClipEnabled(renderer);
  if (clipped) SDL_GetRenderClipRect(renderer, &clip);
  SDL_Rect viewport{};
  SDL_GetRenderViewport(renderer, &viewport);

  int w = 0, h = 0;
  SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
  // The glyphs are 8 by 8 pixels: a whole number of times larger in a
  // larger window, so that they stay sharp and about 1/60 of its height.
  const float k = std::max(1.0f, std::floor(static_cast<float>(h) / 480.0f));
  std::size_t widest = 0;
  for (const std::string& line : lines) widest = std::max(widest, line.size());
  constexpr float kGlyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE, kPad = 3, kLine = kGlyph + 2;
  const SDL_FRect box{4, 4, kPad * 2 + kGlyph * static_cast<float>(widest), kPad * 2 + kLine * static_cast<float>(lines.size()) - 2};
  SDL_SetRenderViewport(renderer, nullptr);
  SDL_SetRenderClipRect(renderer, nullptr);
  SDL_SetRenderScale(renderer, k, k);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
  SDL_RenderFillRect(renderer, &box);
  SDL_SetRenderDrawColor(renderer, 255, 255, 160, 255);
  for (std::size_t i = 0; i < lines.size(); ++i)
    SDL_RenderDebugText(renderer, box.x + kPad, box.y + kPad + kLine * static_cast<float>(i), lines[i].c_str());

  SDL_SetRenderScale(renderer, scale_x, scale_y);
  SDL_SetRenderDrawColor(renderer, r, g, b, a);
  SDL_SetRenderDrawBlendMode(renderer, blend);
  SDL_SetRenderViewport(renderer, &viewport);
  SDL_SetRenderClipRect(renderer, clipped ? &clip : nullptr);
  return SDL_FRect{box.x * k, box.y * k, box.w * k, box.h * k};
}

}  // namespace bd::app
