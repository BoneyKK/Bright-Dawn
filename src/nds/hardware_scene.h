// A frame's 3D scene made ready for the graphics hardware, which draws it
// at any resolution (app/hardware_3d.h). It is made where the program would
// draw the 3D picture itself (PolygonRenderer::build_scene), so the textures
// are read from video memory at the same moment.
//
// Made for speed, not to be exact: the polygons become triangles with a
// depth buffer, and polygons drawn as lines only are left out. Where a frame
// asks for outlines (edge marking) or anti-aliasing, the polygons are also
// handed over as the DS draws them (`raster`), so that those come out as the
// processor draws them.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bd::nds {

// Edge marking (outlines), where a frame asks for it.
enum class EdgeMarking {
  Off,                // Never, even where a frame asks for it.
  OriginalThickness,  // One DS pixel wide, at any resolution. The default.
  OnePixel,           // One pixel of the resolution drawn at.
};

struct HardwareScene {
  // A corner of a triangle, as the graphics hardware reads it (44 bytes).
  struct Vertex {
    float position[4] = {};      // Clip space: x and y from -1 to 1 times w, depth 0 to 1 times w, w.
    float texel[2] = {};         // Texture coordinates, in texels.
    std::uint8_t colour[4] = {};  // Red, green, blue, alpha.
    // 0: the texture's index in `textures` (the drawing side puts the
    // texture's place in its atlas here: x, and y times 65536); 1: its
    // width, and height times 65536; 2: the flags below; 3: the polygon
    // number (0 to 63).
    std::uint32_t info[4] = {};
  };
  // Flags (info[2]).
  static constexpr std::uint32_t kModeMask = 3;       // 0 modulated, 1 decal, 2 toon or highlight.
  static constexpr std::uint32_t kTextured = 1u << 2;
  static constexpr std::uint32_t kRepeatS = 1u << 3;
  static constexpr std::uint32_t kRepeatT = 1u << 4;
  static constexpr std::uint32_t kFlipS = 1u << 5;
  static constexpr std::uint32_t kFlipT = 1u << 6;
  static constexpr std::uint32_t kFog = 1u << 7;      // The polygon takes fog.

  // How a run of triangles is drawn.
  enum class Kind : std::uint8_t {
    Opaque,        // Over what is behind; keeps its depth.
    Translucent,   // Blended by its alpha; keeps its depth.
    Overlay,       // Blended by its alpha; leaves the depth as it was.
    // The pixels of full alpha of a translucent polygon, drawn as opaque ones
    // (as the DS draws them), before its other pixels are drawn as
    // Translucent or Overlay.
    Solid,
    // Shadow polygons, as the DS draws them, with a mark for each pixel (a
    // stencil buffer). Only Shadow draws a colour; none keeps its depth.
    ShadowClear,   // One triangle over the whole picture: no pixel is marked.
    ShadowMask,    // Marks the pixels where it is not nearer than what is there.
    ShadowGuard,   // A shadow's own triangles: unmarks the pixels of opaque polygons of its number.
    Shadow,        // Blended by its alpha where marked and nearer than what is there.
  };
  struct Run {
    std::uint32_t first = 0, count = 0;  // Vertices.
    Kind kind = Kind::Opaque;
    // Drawn where it is nearer than what is there, or also where as near.
    bool depth_equal = false;
    // Its polygon number (0 to 63). Each pixel keeps the number of the last
    // polygon drawn on it, and whether that one is translucent: a
    // translucent run is not drawn over translucent pixels of its number.
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
  // Depths are w, not z: the DS then takes w across a polygon with
  // perspective (as 1 / w goes in proportion across the screen), where z
  // goes in proportion itself.
  bool depth_is_w = false;
  // The toon table: red, green, blue, 0 to 1, for each of 32 shades.
  std::array<std::array<float, 4>, 32> toon{};
  bool highlight = false;    // The table is added rather than used instead.
  // Pixels whose alpha is under this are not drawn (0 to 1).
  float alpha_threshold = 0;
  // Edge marking, as the DS does it: a pixel at the edge of an opaque
  // polygon next to one of another polygon number that is further away
  // (past the screen's edge, the clear plane, with clear_number) takes the
  // colour of its number's group (number / 8), each 0 to 1. One DS pixel
  // wide, or one pixel of the resolution drawn at with edge_one_pixel.
  // With anti-aliasing, an edge pixel is half its colour and half what is
  // behind it, and the edges of polygons are mixed with what is behind them
  // by how much of the pixel they cover. At the DS's resolution both are
  // drawn from `raster`. Above it, outlines are drawn solid from the
  // triangles' own pixels (their polygon numbers and depths), and
  // anti-aliasing is left out.
  bool edge_marking = false;
  bool edge_one_pixel = false;
  bool anti_aliasing = false;
  std::uint8_t clear_number = 0;
  std::array<std::array<float, 4>, 8> edge_colours{};
  // Fog, as the DS does it: a pixel whose polygons all take fog (the clear
  // plane's with clear_fog) is mixed with fog_colour (0 to 1 each, with its
  // alpha) by a density of its depth (24 bits) / 0x200 - fog_offset: the
  // 32 entries of fog_table (0 to 127, 127 counting as 128) are
  // fog_step apart (0x400 >> the shift), the first at one step, with the
  // values between them in proportion. Only the alpha with fog_alpha_only.
  bool fog = false;
  bool fog_alpha_only = false;
  bool clear_fog = false;
  std::array<float, 4> fog_colour{};
  std::uint32_t fog_offset = 0, fog_step = 0;
  std::array<std::uint8_t, 32> fog_table{};

  // The polygons as the DS draws them, with what the drawing needs as the
  // DS has it, for the graphics hardware's own rasterizer
  // (app/shaders/raster.comp), which draws them as the processor does
  // (nds::PolygonRenderer), at any resolution: two layers a pixel, the
  // DS's spans, its edges and how much of a pixel they cover. It is used at
  // the DS's resolution, where every pixel of the scene is taken from it.
  struct Raster {
    // A corner (24 bytes, as the shader reads it).
    struct Corner {
      std::int32_t x = 0, y = 0;   // In sixteenths of a DS pixel.
      std::uint32_t z = 0;         // The depth, as the geometry engine gives it.
      std::int32_t w = 0;
      std::uint32_t st = 0;        // The texture coordinates, s, and t times 65536 (16 bits each).
      std::uint32_t colour = 0;    // Six bits each of red, green and blue.
    };
    // A polygon (32 bytes), its corners in `corners` around it from `first`.
    struct Polygon {
      std::uint32_t first = 0;
      std::uint32_t size = 0;        // 3 to 10.
      std::uint32_t attributes = 0;  // The polygon attribute word.
      std::uint32_t texture = 0;     // The texture parameter word.
      // With kTextured: the texture's index in `textures` (the drawing side
      // puts its place in the atlas here: x, and y times 65536).
      std::uint32_t texture_index = 0;
      std::uint32_t flags = 0;
      std::int32_t w_shift = 0;      // Makes its largest w fit in 16 bits (right; left if negative).
      // Its top and bottom corner's y, in sixteenths of a pixel: top, and
      // bottom times 65536.
      std::uint32_t lines = 0;
    };
    static constexpr std::uint32_t kClockwise = 1;
    static constexpr std::uint32_t kTextured = 2;

    // In the order drawn: the opaque polygons, then the translucent ones.
    std::vector<Corner> corners;
    std::vector<Polygon> polygons;
    std::uint32_t control = 0;          // The DS's 3D control register.
    std::uint32_t alpha_reference = 0;  // For the alpha test, five bits.
    bool depth_is_w = false;
    // The clear plane as a pixel: six bits each of red, green, blue and
    // alpha; its depth (24 bits); whether it is translucent and takes fog.
    std::uint32_t clear_colour = 0, clear_depth = 0;
    bool clear_translucent = false;
    // Six bits each: the toon table (red, green, blue), the edge colours
    // (with alpha 63) and the fog colour (with its alpha).
    std::array<std::uint32_t, 32> toon{};
    std::array<std::uint32_t, 8> edge_colours{};
    std::uint32_t fog_colour = 0;
  };
  Raster raster;
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
