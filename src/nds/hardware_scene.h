// A frame's 3D scene made ready for the graphics hardware, which draws it
// at any resolution (app/hardware_3d.h). It is made where the program would
// draw the 3D picture itself (PolygonRenderer::build_scene), so the textures
// are read from video memory at the same moment.
//
// Made for speed, not to be exact: the polygons become triangles with a
// depth buffer, and fog, edge marking, anti-aliasing, shadow polygons and
// polygons drawn as lines only are left out.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bd::nds {

struct HardwareScene {
  // A corner of a triangle, as the graphics hardware reads it (44 bytes).
  struct Vertex {
    float position[4] = {};      // Clip space: x and y from -1 to 1 times w, depth 0 to 1 times w, w.
    float texel[2] = {};         // Texture coordinates, in texels.
    std::uint8_t colour[4] = {};  // Red, green, blue, alpha.
    // 0: the texture's index in `textures` (the drawing side puts the
    // texture's place in its atlas here: x, and y times 65536); 1: its
    // width, and height times 65536; 2: the flags below; 3: not used.
    std::uint32_t info[4] = {};
  };
  // Flags (info[2]).
  static constexpr std::uint32_t kModeMask = 3;       // 0 modulated, 1 decal, 2 toon or highlight.
  static constexpr std::uint32_t kTextured = 1u << 2;
  static constexpr std::uint32_t kRepeatS = 1u << 3;
  static constexpr std::uint32_t kRepeatT = 1u << 4;
  static constexpr std::uint32_t kFlipS = 1u << 5;
  static constexpr std::uint32_t kFlipT = 1u << 6;

  // How a run of triangles is drawn.
  enum class Kind : std::uint8_t {
    Opaque,        // Over what is behind; keeps its depth.
    Translucent,   // Blended by its alpha; keeps its depth.
    Overlay,       // Blended by its alpha; leaves the depth as it was.
  };
  struct Run {
    std::uint32_t first = 0, count = 0;  // Vertices.
    Kind kind = Kind::Opaque;
    // Drawn where it is nearer than what is there, or also where as near.
    bool depth_equal = false;
    // Of a translucent run: its polygon number plus one. It is not drawn
    // over translucent pixels of the same number (a stencil buffer).
    std::uint8_t number = 0;
  };

  // A texture as red, green, blue and alpha bytes, row after row. The key
  // names its contents: the same key is the same picture.
  struct Texture {
    std::uint64_t key = 0;
    int width = 0, height = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> pixels;
  };

  std::uint64_t serial = 0;  // One more for each scene made.
  std::vector<Vertex> vertices;
  std::vector<Run> runs;
  std::vector<Texture> textures;

  // What is behind every polygon: the clear colour (0 to 1 each, with its
  // alpha) at the clear depth (0 to 1).
  std::array<float, 4> clear_colour{};
  float clear_depth = 1;
  // The toon table: red, green, blue, 0 to 1, for each of 32 shades.
  std::array<std::array<float, 4>, 32> toon{};
  bool highlight = false;    // The table is added rather than used instead.
  // Pixels whose alpha is under this are not drawn (0 to 1).
  float alpha_threshold = 0;
};

// How the graphics hardware makes a display capture (nds::Display), as the
// DS does, at its own resolution: what is taken, and where it goes in its
// bank of video memory. A bank is kept as 256 by 256 pixels, as the main
// engine's display mode 2 shows one: pixel n of the bank at (n % 256,
// n / 256).
struct CaptureRecipe {
  unsigned source = 0;   // 0 the first source, 1 the second, 2 the two blended.
  bool only_3d = false;  // The first source is the 3D scene alone, not the main engine's picture with it.
  unsigned weight_first = 16, weight_second = 0;  // For 2: sixteenths of each.
  unsigned width = 256, height = 192;
  // In pixels: where the capture starts in its bank, and where the second
  // source starts in its bank. Both wrap at the end of the bank.
  std::uint32_t write_start = 0, read_start = 0;
  // The second source's bank: the capture made by the graphics hardware
  // that it holds (its number), or 0, and as it is in video memory (256 *
  // 256 colours of six bits a part; empty if it holds such a capture or the
  // second source is not used).
  std::uint64_t second_capture = 0;
  std::vector<std::uint32_t> second_pixels;
};

}  // namespace bd::nds
