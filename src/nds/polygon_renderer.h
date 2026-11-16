// The back half of the DS's 3D hardware: drawing the polygons of a frame.
//
// The front half (geometry_engine) hands over a frame: polygons with their
// corners placed on the screen. This draws them, 256 by 192 pixels, a line
// at a time, as the hardware does:
// - Each line starts as the clear colour and clear depth.
// - A polygon covers, on a line, the span between its left and its right
//   edge. What a pixel gets (depth, colour, texture coordinates) is taken
//   between the corners in proportion, with perspective: along the two edges
//   first, then across the span.
// - A pixel is drawn where it is nearer than what is there (or as near, for
//   polygons that ask for that). The depth is z, or w if the frame asks.
// - Its colour is the vertex colour, mixed with a texel where the polygon
//   has a texture: modulated, as a decal, or with the toon table (as a
//   replacement or as a highlight). All seven texture formats are read, from
//   the banks of video memory handed to the 3D engine.
// - Opaque polygons are drawn first, then translucent ones, blended over
//   what is there. A translucent pixel is not drawn over one of the same
//   polygon number.
// - Shadow polygons: a mask (polygon number 0) marks where it is hidden
//   behind what is drawn, and the shadow is drawn only there.
// - Polygons with alpha 0 are drawn as outlines.
// - When a line is done: edge marking (outlines between polygons of
//   different numbers), fog by depth, and anti-aliasing of polygon edges
//   against what is behind them.
//
// The registers are GBATEK's; the arithmetic is NooDS's renderer, followed
// step by step, with two additions from GBATEK that NooDS leaves out: the
// switch that turns textures off, and the alpha test.
//
// What is not here:
// - Sorting of translucent polygons. They are drawn in the order sent,
//   whatever the frame asked for.
// - The rear plane as a picture (a bitmap behind everything) and the hiding
//   of far polygons that cover a single dot. A frame that asks for the
//   first is noted.
// - Time. A frame is drawn whole when asked. The hardware draws line by
//   line, ahead of the display, and tells a program when it falls behind;
//   that never happens here.
// - Other resolutions than the hardware's.
//
// One choice that is not the hardware's: textures may be smoothed
// (TextureFilter::Bilinear), each pixel then mixed from the four texels
// nearest to it, where the DS takes the one it falls on. That is for the
// 3D scenes only: polygons drawn without perspective (a game's HUD and
// menus, see GeometryEngine::Polygon::flat) are always drawn texel for
// texel. Textures whose texels are either there or not (all but the two
// formats with an alpha of their own) stay so, keeping their outlines
// sharp: a pixel is drawn where at least half of what it is mixed from is
// there, mixed from what is.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/geometry_engine.h"
#include "nds/video_memory.h"

namespace bd::nds {

class PolygonRenderer {
 public:
  static constexpr int kWidth = 256, kHeight = 192;

  static constexpr std::uint32_t kControl = 0x04000060;         // 16 bits, readable.
  static constexpr std::uint32_t kEdgeColours = 0x04000330;     // Eight of 16 bits.
  static constexpr std::uint32_t kAlphaTestReference = 0x04000340;
  static constexpr std::uint32_t kClearColour = 0x04000350;     // 32 bits.
  static constexpr std::uint32_t kClearDepth = 0x04000354;      // 16 bits.
  static constexpr std::uint32_t kClearImageOffset = 0x04000356;
  static constexpr std::uint32_t kFogColour = 0x04000358;       // 32 bits.
  static constexpr std::uint32_t kFogOffset = 0x0400035C;       // 16 bits.
  static constexpr std::uint32_t kFogTable = 0x04000360;        // 32 bytes.
  static constexpr std::uint32_t kToonTable = 0x04000380;       // 32 of 16 bits.

  // Bits of the control register.
  static constexpr std::uint32_t kTextures = 1u << 0;
  static constexpr std::uint32_t kHighlight = 1u << 1;      // The toon table is added rather than used instead.
  static constexpr std::uint32_t kAlphaTest = 1u << 2;
  static constexpr std::uint32_t kAlphaBlending = 1u << 3;
  static constexpr std::uint32_t kAntiAliasing = 1u << 4;
  static constexpr std::uint32_t kEdgeMarking = 1u << 5;
  static constexpr std::uint32_t kFogAlphaOnly = 1u << 6;
  static constexpr std::uint32_t kFog = 1u << 7;
  static constexpr unsigned kFogShift = 8;                  // Four bits.
  static constexpr std::uint32_t kLinesUnderflow = 1u << 12;  // Write a one to clear. Never set here.
  static constexpr std::uint32_t kRamOverflow = 1u << 13;     // Write a one to clear.
  static constexpr std::uint32_t kRearBitmap = 1u << 14;

  // A drawn pixel: six bits each of red, green, blue and alpha, red lowest.
  // Alpha 0 is a pixel nothing was drawn on (unless the clear colour has an
  // alpha of its own).
  using Image = std::vector<std::uint32_t>;  // kWidth * kHeight, line after line.

  enum class TextureFilter {
    Nearest,   // The texel a pixel falls on, as on the DS. The default.
    Bilinear,  // Mixed from the four nearest texels, in 3D scenes.
  };

  void attach(Arm9Io& io);

  void set_texture_filter(TextureFilter filter) { texture_filter_ = filter; }
  TextureFilter texture_filter() const { return texture_filter_; }

  std::uint32_t control() const { return control_; }
  void write_control(std::uint32_t value, std::uint32_t mask);
  // The front half could not keep a vertex or a polygon: sets the flag a
  // program can read.
  void ram_overflowed() { control_ |= kRamOverflow; }

  // Draws a frame. Textures are read from `video` as it is now.
  const Image& draw(const GeometryEngine::Frame& frame, const VideoMemory& video);
  const Image& image() const { return image_; }

  // State, for tests and reports.
  std::uint32_t clear_colour() const { return clear_colour_; }
  std::uint32_t clear_depth() const { return clear_depth_; }
  std::uint32_t fog_colour() const { return fog_colour_; }
  std::uint32_t fog_offset() const { return fog_offset_; }
  std::uint32_t alpha_test_reference() const { return alpha_reference_; }
  // What a frame asked for that is not carried out, with how many frames.
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  // What is known of a pixel while a frame is drawn.
  struct Pixel {
    std::uint32_t colour = 0;
    std::int32_t depth = 0;
    std::uint8_t opaque_number = 0;       // The polygon number of the opaque pixel,
    std::uint8_t translucent_number = 0;  // and of the last translucent one drawn over it.
    bool translucent = false;
    bool fog = false;
    bool edge = false;
    std::uint8_t edge_alpha = 0x3F;       // How much of the pixel an edge covers, for anti-aliasing.
  };
  // A polygon with what the drawing needs worked out once a frame.
  struct Shape {
    const GeometryEngine::Polygon* polygon = nullptr;
    int top = 0, bottom = 0;       // Lines it is on: from top, before bottom.
    int w_shift = 0;               // Makes its largest w fit in 16 bits.
    std::uint8_t alpha = 0;        // Six bits.
    std::uint8_t mode = 0, number = 0;
    bool fog = false, depth_equal = false, new_depth = false;
    // The texture.
    unsigned format = 0;
    std::uint32_t texture_address = 0, palette_address = 0;
    int size_s = 0, size_t_ = 0;
    bool repeat_s = false, repeat_t = false, flip_s = false, flip_t = false, transparent_0 = false;
    bool smooth = false;           // Its texels mixed (TextureFilter::Bilinear).
  };

  void draw_line(int line);
  void draw_polygon(int line, const Shape& shape);
  void finish_line(int line);
  std::uint32_t texel(const Shape& shape, int s, int t) const;
  // The four texels around a point given in sixteenths of a texel, mixed.
  std::uint32_t smoothed_texel(const Shape& shape, int s16, int t16) const;
  Pixel& at(int layer, int line, int x) { return layers_[layer][static_cast<std::size_t>(line * kWidth + x)]; }

  TextureFilter texture_filter_ = TextureFilter::Nearest;
  std::uint32_t control_ = 0;
  std::array<std::uint16_t, 8> edge_colours_{};
  std::uint32_t alpha_reference_ = 0;
  std::uint32_t clear_colour_ = 0, clear_depth_ = 0, clear_image_offset_ = 0;
  std::uint32_t fog_colour_ = 0, fog_offset_ = 0;
  std::array<std::uint8_t, 32> fog_table_{};
  std::array<std::uint16_t, 32> toon_table_{};

  // While drawing.
  const GeometryEngine::Frame* frame_ = nullptr;
  const VideoMemory* video_ = nullptr;
  bool w_buffer_ = false;
  std::vector<Shape> shapes_;
  // Two layers: what is in front, and at the edges of polygons what is
  // behind, for anti-aliasing.
  std::array<std::vector<Pixel>, 2> layers_;
  std::vector<std::uint8_t> stencil_;
  std::array<bool, kHeight> stencil_cleared_{};
  Image image_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
