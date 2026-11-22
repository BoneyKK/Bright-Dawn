// One frame's 3D, as the program drew it or made a scene of it, written to a
// file and read back: to find, without the game, where the graphics
// hardware's drawing and the processor's part (`bright-dawn --dump-3d`,
// tests/scene_check.cpp).
//
// The file holds the game's textures and screens: it is made on the
// player's computer and is never to be put in the repository.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/polygon_renderer.h"
#include "nds/video_memory.h"

namespace bd::nds {

struct SceneDump {
  PolygonRenderer::Inputs inputs;
  EdgeMarking edge_marking = EdgeMarking::OriginalThickness;
  bool fine_positions = false;
  int scale = 1;  // The graphics hardware's, when it was made.
  // Each screen as handed to the graphics hardware (colours with marks,
  // 256 * 192), and what was put in: 0 nothing, 1 the scene, 2 a capture.
  std::array<std::vector<std::uint32_t>, 2> screens;
  std::array<std::uint8_t, 2> fills{};
  std::array<std::uint16_t, 2> brightness{};
};

bool write_scene_dump(const std::filesystem::path& path, const SceneDump& dump, std::string& error);
bool read_scene_dump(const std::filesystem::path& path, SceneDump& dump, std::string& error);

// Puts the dump's registers into the renderer attached to `io`, and its
// texture memory into `video` (banks A to D as the texture slots, E, F and
// G as the palette slots).
void restore_scene_inputs(const PolygonRenderer::Inputs& inputs, Arm9Io& io, VideoMemory& video);

}  // namespace bd::nds
