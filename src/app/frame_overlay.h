// The frame overlay (launcher, Debug page): a few lines of text on a dark
// box at the window's top-left corner, drawn over the window's picture
// after the screens, never into the game's screens. It leaves the
// renderer's scale, draw colour, blend mode, target and clip as they were.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bd::app {

// What the overlay shows of the last frame-log row: the frame number, the
// host's frame time, the three costliest time columns (in ms) and the GPU's
// name. `names` and `values` are the row (debug/frame_log.h: "frame",
// "total_us", then the columns); `held` marks the game held (Pause and step).
std::vector<std::string> frame_overlay_lines(std::uint64_t frame, const std::vector<std::string>& names,
                                             const std::vector<double>& values, const std::string& gpu_name, bool held);

// Draws `lines` on the current render target; returns the box it drew over,
// in the target's pixels (nothing outside it is changed).
SDL_FRect draw_frame_overlay(SDL_Renderer* renderer, const std::vector<std::string>& lines);

}  // namespace bd::app
