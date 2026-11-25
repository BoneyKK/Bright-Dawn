#include "nds/polygon_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "debug/frame_log.h"

namespace bd::nds {

namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Vertex = GeometryEngine::Vertex;

constexpr u32 kAlphaMask = 0x3Fu << 18;

u32 alpha_of(u32 colour) { return (colour >> 18) & 0x3F; }
u32 part_of(u32 colour, unsigned shift) { return (colour >> shift) & 0x3F; }

// Five bits each of red, green, blue and alpha (alpha from bit 15) made six:
// doubled, and one more unless zero.
u32 six_bits_each(u32 colour) {
  u32 result = 0;
  for (unsigned i = 0; i < 4; ++i) {
    u32 part = ((colour >> (5 * i)) & 0x1F) * 2;
    if (part != 0) ++part;
    result |= part << (6 * i);
  }
  return result;
}

// The value at `x` on the way from `v1` at `x1` to `v2` at `x2`.
u32 linear(u32 v1, u32 v2, u32 x1, u32 x, u32 x2) {
  if (x <= x1) return v1;
  if (x >= x2) return v2;
  if (v1 <= v2) return v1 + (v2 - v1) * (x - x1) / (x2 - x1);
  return v2 + (v1 - v2) * (x2 - x) / (x2 - x1);
}

// The same, worked out from the other end.
u32 linear_reversed(u32 v1, u32 v2, u32 x1, u32 x, u32 x2) {
  if (x <= x1) return v1;
  if (x >= x2) return v2;
  if (v1 <= v2) return v2 - (v2 - v1) * (x2 - x) / (x2 - x1);
  return v1 - (v1 - v2) * (x - x1) / (x2 - x1);
}

// The value a `factor` of the way (out of 1 << shift) from `v1` to `v2`.
u32 by_factor(u32 factor, u32 shift, u32 v1, u32 v2) {
  if (v1 <= v2) return v1 + (((v2 - v1) * factor) >> shift);
  return v2 + (((v1 - v2) * ((1u << shift) - factor)) >> shift);
}

// Red, green and blue on the way from `c1` to `c2`; the larger alpha.
u32 colour_between(u32 c1, u32 c2, u32 x1, u32 x, u32 x2) {
  u32 result = std::max(alpha_of(c1), alpha_of(c2)) << 18;
  for (unsigned shift = 0; shift < 18; shift += 6) result |= linear(part_of(c1, shift), part_of(c2, shift), x1, x, x2) << shift;
  return result;
}

}  // namespace

void PolygonRenderer::attach(Arm9Io& io) {
  io.add(kControl, 2, [this] { return control_; }, [this](u32 value, u32 mask) { write_control(value, mask); });
  for (u32 i = 0; i < 8; ++i)
    io.add(kEdgeColours + 2 * i, 2, nullptr, [this, i](u32 value, u32 mask) {
      mask &= 0x7FFF;
      edge_colours_[i] = static_cast<std::uint16_t>((edge_colours_[i] & ~mask) | (value & mask));
    });
  io.add(kAlphaTestReference, 2, nullptr, [this](u32 value, u32 mask) {
    mask &= 0x1F;
    alpha_reference_ = (alpha_reference_ & ~mask) | (value & mask);
  });
  io.add(kClearColour, 4, nullptr, [this](u32 value, u32 mask) {
    mask &= 0x3F1FFFFF;
    clear_colour_ = (clear_colour_ & ~mask) | (value & mask);
  });
  io.add(kClearDepth, 2, nullptr, [this](u32 value, u32 mask) {
    mask &= 0x7FFF;
    clear_depth_ = (clear_depth_ & ~mask) | (value & mask);
  });
  io.add(kClearImageOffset, 2, nullptr, [this](u32 value, u32 mask) {
    clear_image_offset_ = (clear_image_offset_ & ~mask) | (value & mask);
  });
  io.add(kFogColour, 4, nullptr, [this](u32 value, u32 mask) {
    mask &= 0x001F7FFF;
    fog_colour_ = (fog_colour_ & ~mask) | (value & mask);
  });
  io.add(kFogOffset, 2, nullptr, [this](u32 value, u32 mask) {
    mask &= 0x7FFF;
    fog_offset_ = (fog_offset_ & ~mask) | (value & mask);
  });
  for (u32 i = 0; i < 32; ++i)
    io.add(kFogTable + i, 1, nullptr, [this, i](u32 value, u32 mask) {
      mask &= 0x7F;
      fog_table_[i] = static_cast<std::uint8_t>((fog_table_[i] & ~mask) | (value & mask));
    });
  for (u32 i = 0; i < 32; ++i)
    io.add(kToonTable + 2 * i, 2, nullptr, [this, i](u32 value, u32 mask) {
      mask &= 0x7FFF;
      toon_table_[i] = static_cast<std::uint16_t>((toon_table_[i] & ~mask) | (value & mask));
    });
}

void PolygonRenderer::write_control(std::uint32_t value, std::uint32_t mask) {
  // The two flags are cleared by writing a one.
  control_ &= ~(value & mask & (kLinesUnderflow | kRamOverflow));
  mask &= 0x4FFF;
  control_ = (control_ & ~mask) | (value & mask);
}

void PolygonRenderer::make_shapes(const GeometryEngine::Frame& frame) {
  shapes_.clear();
  for (const GeometryEngine::Polygon& polygon : frame.polygons) {
    if (polygon.size < 3 || polygon.size > 10 || polygon.first + std::size_t{polygon.size} > frame.vertices.size()) continue;
    const Vertex* corner = &frame.vertices[polygon.first];
    Shape shape;
    shape.polygon = &polygon;

    // The lines it is on. One that is all on one line is drawn on that line.
    shape.top = shape.bottom = corner[0].y;
    u32 largest_w = static_cast<u32>(corner[0].w);
    for (unsigned i = 1; i < polygon.size; ++i) {
      shape.top = std::min(shape.top, corner[i].y);
      shape.bottom = std::max(shape.bottom, corner[i].y);
      largest_w = std::max(largest_w, static_cast<u32>(corner[i].w));
    }
    if (shape.top == shape.bottom) ++shape.bottom;

    // w is brought to 16 bits for the arithmetic: down in steps of four bits
    // while it does not fit, or up while it still does.
    while ((largest_w >> shape.w_shift) > 0xFFFF) shape.w_shift += 4;
    if (shape.w_shift == 0 && largest_w != 0) {
      while ((static_cast<u64>(largest_w) << (4 - shape.w_shift)) <= 0xFFFF) shape.w_shift -= 4;
    }

    const u32 attributes = polygon.attributes;
    shape.mode = static_cast<std::uint8_t>((attributes >> 4) & 3);
    shape.new_depth = (attributes & (1u << 11)) != 0;
    shape.depth_equal = (attributes & (1u << 14)) != 0;
    shape.fog = (attributes & (1u << 15)) != 0;
    const u32 alpha = (attributes >> 16) & 0x1F;
    shape.alpha = static_cast<std::uint8_t>(alpha == 0 ? 0 : alpha * 2 + 1);
    shape.number = static_cast<std::uint8_t>((attributes >> 24) & 0x3F);

    const u32 texture = polygon.texture;
    shape.format = (control_ & kTextures) != 0 ? (texture >> 26) & 7 : 0;
    shape.texture_address = (texture & 0xFFFF) << 3;
    shape.size_s = 8 << ((texture >> 20) & 7);
    shape.size_t_ = 8 << ((texture >> 23) & 7);
    shape.repeat_s = (texture & (1u << 16)) != 0;
    shape.repeat_t = (texture & (1u << 17)) != 0;
    shape.flip_s = (texture & (1u << 18)) != 0;
    shape.flip_t = (texture & (1u << 19)) != 0;
    shape.transparent_0 = (texture & (1u << 29)) != 0;
    // The palette's address is in units of 16 bytes, of 8 for palettes of
    // four colours.
    shape.palette_address = polygon.palette << (shape.format == 2 ? 3 : 4);
    shape.smooth = texture_filter_ == TextureFilter::Bilinear && shape.format != 0 && !polygon.flat;
    shapes_.push_back(shape);
  }
}

void PolygonRenderer::keep(const GeometryEngine::Frame& frame, const VideoMemory& video) {
  Inputs& in = inputs_;
  in.frame = frame;
  in.control = control_;
  in.alpha_reference = alpha_reference_;
  in.clear_colour = clear_colour_;
  in.clear_depth = clear_depth_;
  in.clear_image_offset = clear_image_offset_;
  in.fog_colour = fog_colour_;
  in.fog_offset = fog_offset_;
  in.edge_colours = edge_colours_;
  in.fog_table = fog_table_;
  in.toon_table = toon_table_;
  in.textures.resize(VideoMemory::kTextureSize);
  for (u32 a = 0; a < VideoMemory::kTextureSize; ++a) in.textures[a] = static_cast<std::int16_t>(video.texture(a));
  in.palettes.resize(VideoMemory::kTexturePaletteSize);
  for (u32 a = 0; a < VideoMemory::kTexturePaletteSize; ++a)
    in.palettes[a] = static_cast<std::int16_t>(video.texture_palette(a));
}

const PolygonRenderer::Image& PolygonRenderer::draw(const GeometryEngine::Frame& frame, const VideoMemory& video) {
  if (keep_inputs_) keep(frame, video);
  frame_ = &frame;
  video_ = &video;
  w_buffer_ = frame.depth_is_w;
  if ((control_ & kRearBitmap) != 0) ++notes_["the rear plane as a picture"];
  make_shapes(frame);
  {
    frame_log::Counts& c = frame_log::state().counts;
    c.polygons = frame.polygons.size();
    c.vertices = frame.vertices.size();
    c.edge_marking = (control_ & kEdgeMarking) != 0 && edge_marking_ != EdgeMarking::Off;
    c.anti_aliasing = (control_ & kAntiAliasing) != 0;
    c.fog = (control_ & kFog) != 0;
  }

  const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
  for (std::vector<Pixel>& layer : layers_) layer.assign(pixels, Pixel{});
  stencil_.assign(pixels, 0);
  if (!workers_) workers_ = std::make_unique<core::Workers>();
  workers_->for_each(kHeight, [this](std::size_t line) { draw_line(static_cast<int>(line)); });
  workers_->for_each(kHeight, [this](std::size_t line) { finish_line(static_cast<int>(line)); });

  image_.resize(pixels);
  for (std::size_t i = 0; i < pixels; ++i) image_[i] = layers_[0][i].colour;
  frame_ = nullptr;
  video_ = nullptr;
  return image_;
}

namespace {

// The FNV-1a hash, a number at a time.
u64 hashed(u64 hash, u32 value) { return (hash ^ value) * 0x100000001B3ull; }
// Six bits to eight.
u32 eight_bits(u32 six) { return ((six & 0x3F) << 2) | ((six & 0x3F) >> 4); }
float fraction_of_31(u32 five) { return static_cast<float>(five & 0x1F) / 31.0f; }

}  // namespace

std::uint32_t PolygonRenderer::scene_texture(const Shape& shape) {
  // The texels depend on the texture word (but for repeating and flipping)
  // and the palette: a texture used again in the scene is looked at once.
  const u32 word = shape.polygon->texture & ~0x000F0000u;
  const u64 words = u64{word} << 32 | shape.polygon->palette;
  if (const auto it = scene_textures_.find(words); it != scene_textures_.end()) return it->second;

  // What the texture is made of: its bytes and its palette's, hashed.
  const VideoMemory& video = *video_;
  u64 hash = hashed(hashed(0xCBF29CE484222325ull, word), shape.palette_address);
  static constexpr u32 kBits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
  const u32 texels = static_cast<u32>(shape.size_s * shape.size_t_);
  const u32 bytes = texels * kBits[shape.format] / 8;
  for (u32 i = 0; i < bytes; ++i) hash = hashed(hash, static_cast<u32>(video.texture(shape.texture_address + i)));
  static constexpr u32 kColours[8] = {0, 32, 4, 16, 256, 0, 8, 0};
  u32 colours = kColours[shape.format];
  if (shape.format == 5) {
    // The table of each block of four by four texels, and the palette as
    // far as the table reaches into it.
    const u32 table = 0x20000 + (shape.texture_address % 0x20000) / 2 + (shape.texture_address / 0x20000 == 2 ? 0x10000 : 0);
    u32 largest = 0;
    for (u32 block = 0; block < texels / 16; ++block) {
      const int low = video.texture(table + block * 2), high = video.texture(table + block * 2 + 1);
      hash = hashed(hashed(hash, static_cast<u32>(low)), static_cast<u32>(high));
      if (low >= 0 && high >= 0) largest = std::max(largest, static_cast<u32>(low | high << 8) & 0x3FFF);
    }
    colours = largest * 2 + 4;
  }
  for (u32 i = 0; i < colours * 2; ++i)
    hash = hashed(hash, static_cast<u32>(video.texture_palette(shape.palette_address + i)));

  auto found = decoded_.find(hash);
  if (found == decoded_.end()) {
    auto pixels = std::make_shared<std::vector<u32>>(texels);
    for (int t = 0; t < shape.size_t_; ++t) {
      for (int s = 0; s < shape.size_s; ++s) {
        const u32 c = texel(shape, s, t);
        (*pixels)[static_cast<std::size_t>(t * shape.size_s + s)] =
            eight_bits(c) | eight_bits(c >> 6) << 8 | eight_bits(c >> 12) << 16 | eight_bits(c >> 18) << 24;
      }
    }
    decoded_bytes_ += std::size_t{texels} * 4;
    ++frame_log::state().counts.textures_decoded;
    frame_log::state().counts.decoded_bytes += std::size_t{texels} * 4;
    found = decoded_.emplace(hash, Decoded{std::move(pixels), shape.size_s, shape.size_t_, 0}).first;
  }
  found->second.last_used = scene_.serial;
  scene_.textures.push_back({hash, found->second.width, found->second.height, found->second.pixels});
  const auto index = static_cast<std::uint32_t>(scene_.textures.size() - 1);
  scene_textures_.emplace(words, index);
  return index;
}

const HardwareScene& PolygonRenderer::build_scene(const GeometryEngine::Frame& frame, const VideoMemory& video, bool wide) {
  if (keep_inputs_) keep(frame, video);
  frame_ = &frame;
  video_ = &video;
  make_shapes(frame);
  {
    frame_log::Counts& c = frame_log::state().counts;
    c.polygons = frame.polygons.size();
    c.vertices = frame.vertices.size();
    c.edge_marking = (control_ & kEdgeMarking) != 0 && edge_marking_ != EdgeMarking::Off;
    c.anti_aliasing = (control_ & kAntiAliasing) != 0;
    c.fog = (control_ & kFog) != 0;
  }
  HardwareScene& scene = scene_;
  ++scene.serial;
  scene.wide = wide;
  scene.vertices.clear();
  scene.runs.clear();
  pair_.clear();
  pair_runs_ = static_cast<std::size_t>(-1);
  scene.textures.clear();
  scene_textures_.clear();

  for (unsigned i = 0; i < 3; ++i) scene.clear_colour[i] = fraction_of_31(clear_colour_ >> (5 * i));
  scene.clear_colour[3] = fraction_of_31(clear_colour_ >> 16);
  scene.depth_is_w = frame.depth_is_w;
  scene.clear_depth = clear_depth_ == 0x7FFF ? 1.0f : static_cast<float>(clear_depth_ << 9) / 16777216.0f;
  for (std::size_t i = 0; i < scene.toon.size(); ++i)
    for (unsigned part = 0; part < 3; ++part) scene.toon[i][part] = fraction_of_31(toon_table_[i] >> (5 * part));
  scene.highlight = (control_ & kHighlight) != 0;
  // Alpha, six bits: 0 is never drawn; with the alpha test, nor is what has
  // five bits of alpha at or under the reference.
  scene.edge_marking = (control_ & kEdgeMarking) != 0 && edge_marking_ != EdgeMarking::Off;
  scene.edge_one_pixel = edge_marking_ == EdgeMarking::OnePixel;
  scene.anti_aliasing = (control_ & kAntiAliasing) != 0;
  scene.clear_number = static_cast<std::uint8_t>((clear_colour_ >> 24) & 0x3F);
  for (std::size_t i = 0; i < scene.edge_colours.size(); ++i) {
    for (unsigned part = 0; part < 3; ++part) scene.edge_colours[i][part] = fraction_of_31(edge_colours_[i] >> (5 * part));
    scene.edge_colours[i][3] = 1;
  }
  scene.fog = (control_ & kFog) != 0;
  scene.fog_alpha_only = (control_ & kFogAlphaOnly) != 0;
  scene.clear_fog = (clear_colour_ & 0x8000) != 0;
  for (unsigned part = 0; part < 3; ++part) scene.fog_colour[part] = fraction_of_31(fog_colour_ >> (5 * part));
  scene.fog_colour[3] = fraction_of_31(fog_colour_ >> 16);
  scene.fog_offset = fog_offset_;
  scene.fog_step = 0x400u >> ((control_ >> kFogShift) & 0xF);
  scene.fog_table = fog_table_;
  scene.alpha_threshold = ((control_ & kAlphaTest) != 0 ? static_cast<float>(alpha_reference_ & 0x1F) * 2 + 1.5f : 0.5f) / 63.0f;

  // The polygons as the DS draws them, with the registers as the
  // processor's drawing reads them.
  HardwareScene::Raster& raster = scene.raster;
  raster.corners.clear();
  raster.polygons.clear();
  raster.control = control_;
  raster.alpha_reference = alpha_reference_ & 0x1F;
  raster.depth_is_w = frame.depth_is_w;
  const u32 clear_alpha = (clear_colour_ >> 16) & 0x1F;
  raster.clear_colour = six_bits_each((clear_alpha << 15) | (clear_colour_ & 0x7FFF));
  raster.clear_depth = clear_depth_ == 0x7FFF ? 0xFFFFFF : clear_depth_ << 9;
  raster.clear_translucent = clear_alpha != 0 && clear_alpha < 31;
  for (std::size_t i = 0; i < raster.toon.size(); ++i) raster.toon[i] = six_bits_each(toon_table_[i] & 0x7FFF);
  for (std::size_t i = 0; i < raster.edge_colours.size(); ++i)
    raster.edge_colours[i] = six_bits_each((0x1Fu << 15) | edge_colours_[i]);
  raster.fog_colour = six_bits_each((((fog_colour_ >> 16) & 0x1F) << 15) | (fog_colour_ & 0x7FFF));

  // Opaque polygons first, then the translucent ones, each in the order sent.
  // A run of shadow masks starts with no pixel marked, as on the DS.
  using Kind = HardwareScene::Kind;
  bool after_mask = false;
  const auto add = [&](bool translucent_pass) {
    for (const Shape& shape : shapes_) {
      const bool translucent = shape.alpha < 0x3F || shape.format == 1 || shape.format == 6;
      if (translucent != translucent_pass) continue;
      const bool shadow = shape.mode == 3;
      const bool mask = shadow && shape.number == 0;
      // Left out: polygons drawn as outlines.
      if (shape.alpha == 0) {
        after_mask = false;
        continue;
      }
      const Kind kind = mask              ? Kind::ShadowMask
                        : shadow          ? Kind::Shadow
                        : !translucent    ? Kind::Opaque
                        : shape.new_depth ? Kind::Translucent
                                          : Kind::Overlay;
      // A shadow is coloured as a modulated polygon; a mask is not drawn,
      // so nothing of it is left out for its alpha.
      u32 info[4] = {0, 0, (shadow ? 0u : shape.mode) | (shape.fog ? HardwareScene::kFog : 0u), shape.number};
      const GeometryEngine::Polygon& polygon = *shape.polygon;
      // Polygons drawn without perspective (a HUD, a menu, a message box)
      // stay at the DS's whole pixels: their pieces are made to meet there,
      // and between them they can leave gaps of less than a pixel, which a
      // larger picture would show.
      const bool fine = fine_positions_ && !polygon.flat;
      HardwareScene::Raster::Polygon& drawn = raster.polygons.emplace_back();
      drawn.first = static_cast<u32>(raster.corners.size());
      drawn.size = polygon.size;
      drawn.attributes = polygon.attributes;
      drawn.texture = polygon.texture;
      drawn.flags = polygon.clockwise ? HardwareScene::Raster::kClockwise : 0;
      drawn.w_shift = shape.w_shift;
      i32 top = 0x7FFFFFFF, bottom = 0;
      for (unsigned i = 0; i < polygon.size; ++i) {
        // An uncut polygon of a strip is stored in the strip's order.
        const unsigned k = polygon.strip_order && (i == 2 || i == 3) ? 5 - i : i;
        const Vertex& v = frame.vertices[polygon.first + std::size_t{k}];
        HardwareScene::Raster::Corner& c = raster.corners.emplace_back();
        c.x = fine ? v.fine_x : v.x * 16;
        c.y = fine ? v.fine_y : v.y * 16;
        c.z = static_cast<u32>(v.z);
        c.w = v.w;
        c.st = static_cast<std::uint16_t>(v.s) | static_cast<u32>(static_cast<std::uint16_t>(v.t)) << 16;
        c.colour = v.colour & 0x3FFFF;
        top = std::min(top, c.y);
        bottom = std::max(bottom, c.y);
      }
      drawn.lines = static_cast<u32>(top) | static_cast<u32>(bottom) << 16;
      if (shape.format != 0 && !mask) {
        info[0] = scene_texture(shape);
        drawn.texture_index = info[0];
        drawn.flags |= HardwareScene::Raster::kTextured;
        info[1] = static_cast<u32>(shape.size_s) | static_cast<u32>(shape.size_t_) << 16;
        info[2] |= HardwareScene::kTextured | (shape.repeat_s ? HardwareScene::kRepeatS : 0) |
                   (shape.repeat_t ? HardwareScene::kRepeatT : 0) | (shape.flip_s ? HardwareScene::kFlipS : 0) |
                   (shape.flip_t ? HardwareScene::kFlipT : 0);
      }
      HardwareScene::Vertex corners[10];
      for (unsigned i = 0; i < polygon.size; ++i) {
        const Vertex& v = frame.vertices[polygon.first + std::size_t{i}];
        HardwareScene::Vertex& c = corners[i];
        const float w = v.w > 0 ? static_cast<float>(v.w) / 4096.0f : 1.0f / 4096.0f;
        const float x = fine ? static_cast<float>(v.fine_x) / 16.0f : static_cast<float>(v.x);
        const float y = fine ? static_cast<float>(v.fine_y) / 16.0f : static_cast<float>(v.y);
        c.position[0] = (x / 128.0f - 1.0f) * w;
        c.position[1] = (1.0f - y / 96.0f) * w;
        // The depth the frame asks for: z, or w (as the processor's drawing
        // has it, 24 bits). Polygons made with other matrices, or whose sums
        // wrapped, are in a different order by z than by w.
        const float depth = frame.depth_is_w ? static_cast<float>(std::clamp(v.w, 0, 0xFFFFFF))
                                             : static_cast<float>(v.z);
        c.position[2] = std::clamp(depth / 16777216.0f, 0.0f, 1.0f) * w;
        c.position[3] = w;
        c.texel[0] = static_cast<float>(v.s) / 16.0f;
        c.texel[1] = static_cast<float>(v.t) / 16.0f;
        c.colour[0] = static_cast<std::uint8_t>(eight_bits(v.colour));
        c.colour[1] = static_cast<std::uint8_t>(eight_bits(v.colour >> 6));
        c.colour[2] = static_cast<std::uint8_t>(eight_bits(v.colour >> 12));
        c.colour[3] = static_cast<std::uint8_t>(mask ? 0xFF : eight_bits(shape.alpha));
        std::copy(std::begin(info), std::end(info), c.info);
      }
      // An uncut polygon of a strip of four-cornered polygons is stored in
      // the strip's order.
      if (polygon.strip_order) std::swap(corners[2], corners[3]);
      const auto number = static_cast<std::uint8_t>(shape.number);
      if (mask && !after_mask) {
        // One triangle over the whole picture.
        scene.runs.push_back({static_cast<u32>(scene.vertices.size()), 3, Kind::ShadowClear, false, 0});
        for (const float xy : {0.0f, 1.0f, 2.0f}) {
          HardwareScene::Vertex& v = scene.vertices.emplace_back();
          v.position[0] = xy == 1.0f ? 3.0f : -1.0f;
          v.position[1] = xy == 2.0f ? 3.0f : -1.0f;
          v.position[3] = 1.0f;
          v.colour[3] = 0xFF;
        }
      }
      after_mask = mask;
      const auto put = [&](Kind k) {
        if (scene.runs.empty() || scene.runs.back().kind != k || scene.runs.back().depth_equal != shape.depth_equal ||
            scene.runs.back().number != number)
          scene.runs.push_back({static_cast<u32>(scene.vertices.size()), 0, k, shape.depth_equal, number});
        // A fan of triangles from the first corner.
        for (unsigned i = 1; i + 1 < polygon.size; ++i) {
          scene.vertices.push_back(corners[0]);
          scene.vertices.push_back(corners[i]);
          scene.vertices.push_back(corners[i + 1]);
          scene.runs.back().count += 3;
        }
      };
      if (kind == Kind::Shadow) put(Kind::ShadowGuard);
      // A translucent polygon of full alpha has pixels of full alpha where
      // its texture has: those are drawn as opaque ones, first, then the
      // same triangles again for the other pixels. Polygons after one
      // another that share no pixel go in the same pair of runs
      // (nds/polygon_outline.h): a draw call for many, not two for each.
      if ((kind == Kind::Translucent || kind == Kind::Overlay) && shape.alpha == 0x3F) {
        PolygonOutline outline;
        outline.size = polygon.size;
        for (unsigned i = 0; i < polygon.size; ++i) {
          const unsigned k = polygon.strip_order && (i == 2 || i == 3) ? 5 - i : i;
          const Vertex& v = frame.vertices[polygon.first + std::size_t{k}];
          outline.x[i] = fine ? v.fine_x : v.x * 16;
          outline.y[i] = fine ? v.fine_y : v.y * 16;
        }
        outline.bound();
        const std::size_t runs = scene.runs.size();
        bool joins = runs >= 2 && pair_runs_ == runs - 2 && pair_.size() < kMostInPair;
        if (joins) {
          const HardwareScene::Run& solid = scene.runs[runs - 2];
          const HardwareScene::Run& other = scene.runs[runs - 1];
          joins = other.kind == kind && other.number == number && other.depth_equal == shape.depth_equal &&
                  solid.first + solid.count == scene.vertices.size() &&
                  std::all_of(pair_.begin(), pair_.end(),
                              [&](const PolygonOutline& o) { return drawn_apart(o, outline); });
        }
        if (joins) {
          for (unsigned i = 1; i + 1 < polygon.size; ++i) {
            scene.vertices.push_back(corners[0]);
            scene.vertices.push_back(corners[i]);
            scene.vertices.push_back(corners[i + 1]);
          }
          const u32 added = 3 * (polygon.size - 2);
          scene.runs[runs - 2].count += added;
          scene.runs[runs - 1].count += added;
        } else {
          put(Kind::Solid);
          const HardwareScene::Run& solid = scene.runs.back();
          scene.runs.push_back({solid.first, solid.count, kind, shape.depth_equal, number});
          pair_runs_ = scene.runs.size() - 2;
          pair_.clear();
        }
        pair_.push_back(outline);
      } else {
        put(kind);
      }
    }
  };
  add(false);
  add(true);

  // Textures not used for a second are forgotten once many are kept.
  if (decoded_bytes_ > (std::size_t{64} << 20)) {
    for (auto it = decoded_.begin(); it != decoded_.end();) {
      if (it->second.last_used + 60 < scene.serial) {
        decoded_bytes_ -= it->second.pixels->size() * 4;
        it = decoded_.erase(it);
      } else {
        ++it;
      }
    }
  }
  frame_log::state().counts.scene_textures = scene.textures.size();
  frame_log::state().counts.gpu_vertices = scene.vertices.size();
  frame_log::state().counts.scene_draws = scene.runs.size();
  frame_ = nullptr;
  video_ = nullptr;
  return scene;
}

void PolygonRenderer::draw_line(int line) {
  // What is behind everything: the clear colour (with its own alpha, fog
  // flag and polygon number) at the clear depth.
  Pixel clear;
  const u32 clear_alpha = (clear_colour_ >> 16) & 0x1F;
  clear.colour = six_bits_each((clear_alpha << 15) | (clear_colour_ & 0x7FFF));
  clear.depth = clear_depth_ == 0x7FFF ? 0xFFFFFF : static_cast<i32>(clear_depth_ << 9);
  clear.opaque_number = clear.translucent_number = static_cast<std::uint8_t>((clear_colour_ >> 24) & 0x3F);
  clear.translucent = clear_alpha != 0 && clear_alpha < 31;
  clear.fog = (clear_colour_ & 0x8000) != 0;
  for (int x = 0; x < kWidth; ++x) at(0, line, x) = at(1, line, x) = clear;
  stencil_cleared_[static_cast<std::size_t>(line)] = false;

  // Opaque polygons first, then the translucent ones, each in the order
  // sent. Textures with an alpha of their own make a polygon translucent.
  const auto translucent = [](const Shape& shape) { return shape.alpha < 0x3F || shape.format == 1 || shape.format == 6; };
  for (const Shape& shape : shapes_)
    if (line >= shape.top && line < shape.bottom && !translucent(shape)) draw_polygon(line, shape);
  for (const Shape& shape : shapes_)
    if (line >= shape.top && line < shape.bottom && translucent(shape)) draw_polygon(line, shape);
}

void PolygonRenderer::draw_polygon(int line, const Shape& shape) {
  const GeometryEngine::Polygon& polygon = *shape.polygon;
  const int size = polygon.size;
  const u32 uline = static_cast<u32>(line);

  const Vertex* vertices[10] = {};
  for (int i = 0; i < size; ++i) vertices[i] = &frame_->vertices[polygon.first + static_cast<std::size_t>(i)];
  // An uncut polygon of a strip of four-cornered polygons is stored in the
  // strip's order.
  if (polygon.strip_order) std::swap(vertices[2], vertices[3]);

  // The topmost corner, and from it the two edges that cross this line: one
  // found going forwards around the polygon, one going backwards.
  int start = 0;
  for (int i = 0; i < size; ++i)
    if (vertices[start]->y > vertices[i]->y) start = i;
  int v[4] = {start, (start + 1) % size, start, (start - 1 + size) % size};
  while (vertices[v[1]]->y <= line) {
    v[0] = v[1];
    v[1] = (v[1] + 1) % size;
    if (v[0] == start) break;  // All the way round: no edge crosses.
  }
  while (vertices[v[3]]->y <= line) {
    v[2] = v[3];
    v[3] = (v[3] - 1 + size) % size;
    if (v[2] == start) break;
  }
  // Which is the left one depends on which way round the polygon runs.
  if (polygon.clockwise) {
    std::swap(v[0], v[2]);
    std::swap(v[1], v[3]);
  }
  // Each edge from its upper end to its lower.
  if (vertices[v[0]]->y > vertices[v[1]]->y) std::swap(v[0], v[1]);
  if (vertices[v[2]]->y > vertices[v[3]]->y) std::swap(v[2], v[3]);

  const auto X = [&](int i) { return vertices[v[i]]->x; };
  const auto Y = [&](int i) { return vertices[v[i]]->y; };
  const auto ux = [&](int i) { return static_cast<u32>(vertices[v[i]]->x); };
  const auto uy = [&](int i) { return static_cast<u32>(vertices[v[i]]->y); };
  const bool anti_aliasing = (control_ & kAntiAliasing) != 0;

  // The pixels each edge covers on this line: x1 to x2 the left edge, x3 to
  // x4 the right. An edge that runs more across than down covers several.
  // With anti-aliasing, how much of its first and last pixel it covers.
  u32 x1, x2, x3, x4;
  u32 x1a = 0x3F, x2a = 0x3F, x3a = 0x3F, x4a = 0x3F;

  if (Y(0) == Y(1)) {  // Level.
    x1 = ux(0);
    x2 = ux(1);
    if (X(0) > X(1)) {
      std::swap(v[0], v[1]);
      std::swap(x1, x2);
    }
  } else if (std::abs(X(1) - X(0)) > Y(1) - Y(0)) {  // More across than down.
    // With one more bit, so that the result can be rounded.
    x1 = linear(ux(0) << 1, ux(1) << 1, uy(0), uline, uy(1)) + 1;
    x2 = linear(ux(0) << 1, ux(1) << 1, uy(0), uline + 1, uy(1)) + 1;
    const bool negative = X(0) > X(1);
    if (negative) {
      std::swap(v[0], v[1]);
      std::swap(x1, x2);
    }
    if (anti_aliasing) {
      x1a = linear(uy(0) << 6, uy(1) << 6, ux(0) << 1, x1, ux(1) << 1) & 0x3F;
      x2a = linear(uy(0) << 6, uy(1) << 6, ux(0) << 1, x2 - 2, ux(1) << 1) & 0x3F;
      if (negative) {
        x1a = 0x3F - x1a;
        x2a = 0x3F - x2a;
      }
    }
    x1 >>= 1;
    x2 >>= 1;
  } else {  // More down than across.
    // With six more bits, for the coverage. Edges that lean left are worked
    // out from the other end.
    if (X(0) > X(1))
      x1 = linear_reversed(ux(0) << 6, ux(1) << 6, uy(0), uline, uy(1)) - 1;
    else
      x1 = linear(ux(0) << 6, ux(1) << 6, uy(0), uline, uy(1));
    if (anti_aliasing) {
      if (std::abs(X(1) - X(0)) == Y(1) - Y(0))
        x2a = x1a = 0x20;
      else
        x2a = x1a = 0x3F - (x1 & 0x3F);
    }
    x2 = x1 >>= 6;
  }

  if (Y(2) == Y(3)) {  // Level.
    x3 = ux(2);
    x4 = ux(3);
    if (X(2) > X(3)) {
      std::swap(v[2], v[3]);
      std::swap(x3, x4);
    }
  } else if (X(2) == X(3)) {  // Upright.
    // An upright right edge is one pixel further left, unless the polygon is
    // a line.
    x3 = ux(2);
    if ((X(0) != X(1) || X(0) != X(2)) && x3 > 0) --x3;
    x4 = x3;
  } else if (std::abs(X(3) - X(2)) > Y(3) - Y(2)) {  // More across than down.
    x3 = linear(ux(2) << 1, ux(3) << 1, uy(2), uline, uy(3)) + 1;
    x4 = linear(ux(2) << 1, ux(3) << 1, uy(2), uline + 1, uy(3)) + 1;
    const bool negative = X(2) > X(3);
    if (negative) {
      std::swap(v[2], v[3]);
      std::swap(x3, x4);
    }
    if (anti_aliasing) {
      x3a = linear(uy(2) << 6, uy(3) << 6, ux(2) << 1, x3, ux(3) << 1) & 0x3F;
      x4a = linear(uy(2) << 6, uy(3) << 6, ux(2) << 1, x4 - 2, ux(3) << 1) & 0x3F;
      if (!negative) {
        x3a = 0x3F - x3a;
        x4a = 0x3F - x4a;
      }
    }
    x3 >>= 1;
    x4 >>= 1;
  } else {  // More down than across.
    if (X(2) > X(3))
      x3 = linear_reversed(ux(2) << 6, ux(3) << 6, uy(2), uline, uy(3)) - 1;
    else
      x3 = linear(ux(2) << 6, ux(3) << 6, uy(2), uline, uy(3));
    if (anti_aliasing) {
      if (std::abs(X(3) - X(2)) == Y(3) - Y(2))
        x4a = x3a = 0x20;
      else
        x4a = x3a = x3 & 0x3F;
    }
    x4 = x3 >>= 6;
  }

  // An edge ends before the next line's begins.
  if (x2 > x1) --x2;
  if (x4 > x3) --x4;

  // Edges that cross each other.
  if (x2 > x3) x2 = x3;
  if (x1 > x2) {
    x2 = x1;
    x1a = 0x3F - x1a;
    x2a = 0x3F - x2a;
  }
  if (x2 > x3) x3 = x2;
  if (x3 > x4) {
    x3 = x4;
    x3a = 0x3F - x3a;
    x4a = 0x3F - x4a;
  }
  // The leftmost edge first.
  if (x1 > x4) {
    std::swap(v[0], v[2]);
    std::swap(v[1], v[3]);
    std::swap(x1, x3);
    std::swap(x2, x4);
    std::swap(x1a, x3a);
    std::swap(x2a, x4a);
  }

  // How far along each edge this line is: measured across for an edge that
  // runs more across than down, and down otherwise, whichever is finer.
  // Edges whose pixels are left out of opaque polygons, so that polygons
  // side by side do not draw the pixels between them twice.
  u32 xe1[2], xe[2], xe2[2];
  bool hide_left, hide_right;
  if (std::abs(X(1) - X(0)) > std::abs(Y(1) - Y(0))) {
    xe1[0] = ux(0);
    xe[0] = x1;
    xe2[0] = ux(1);
    hide_left = Y(0) < Y(1);
  } else {
    xe1[0] = uy(0);
    xe[0] = uline;
    xe2[0] = uy(1);
    hide_left = false;
  }
  if (std::abs(X(3) - X(2)) > std::abs(Y(3) - Y(2))) {
    xe1[1] = ux(2);
    xe[1] = x4;
    xe2[1] = ux(3);
    hide_right = Y(2) > Y(3);
  } else {
    xe1[1] = uy(2);
    xe[1] = uline;
    xe2[1] = uy(3);
    hide_right = X(2) != X(3);
  }

  u32 ws[4];
  for (int i = 0; i < 4; ++i) {
    const i32 w = vertices[v[i]]->w;
    ws[i] = shape.w_shift >= 0 ? static_cast<u32>(w >> shape.w_shift) : static_cast<u32>(w) << -shape.w_shift;
  }

  // At the two ends of the span: depth, w, the vertex colour (with three
  // more bits) and the texture coordinates (made unsigned for the
  // arithmetic).
  u32 ze[2], we[2], re[2], ge[2], be[2], se[2], te[2];
  const auto colour_part = [&](int i, unsigned shift) { return part_of(vertices[v[i]]->colour, shift) << 3; };
  const auto s_of = [&](int i) { return static_cast<u32>(i32{vertices[v[i]]->s} + 0xFFFF); };
  const auto t_of = [&](int i) { return static_cast<u32>(i32{vertices[v[i]]->t} + 0xFFFF); };
  for (int i = 0; i < 2; ++i) {
    const int a = i * 2, b = a + 1;
    ze[i] = linear(static_cast<u32>(vertices[v[a]]->z), static_cast<u32>(vertices[v[b]]->z), xe1[i], xe[i], xe2[i]);
    if (ws[a] == ws[b] && (ws[a] & 0x00FE) == 0) {
      // No perspective to allow for.
      we[i] = linear(ws[a], ws[b], xe1[i], xe[i], xe2[i]);
      re[i] = linear(colour_part(a, 0), colour_part(b, 0), xe1[i], xe[i], xe2[i]);
      ge[i] = linear(colour_part(a, 6), colour_part(b, 6), xe1[i], xe[i], xe2[i]);
      be[i] = linear(colour_part(a, 12), colour_part(b, 12), xe1[i], xe[i], xe2[i]);
      se[i] = linear(s_of(a), s_of(b), xe1[i], xe[i], xe2[i]) - 0xFFFF;
      te[i] = linear(t_of(a), t_of(b), xe1[i], xe[i], xe2[i]) - 0xFFFF;
    } else {
      // How far along the edge, with perspective, in 512ths.
      u32 factor;
      if (xe[i] <= xe1[i]) {
        factor = 0;
      } else if (xe[i] >= xe2[i]) {
        factor = 1u << 9;
      } else {
        const u32 wa = (ws[a] >> 1) + (((ws[a] & 1) != 0 && (ws[b] & 1) == 0) ? 1u : 0u);
        const u32 below = (ws[b] >> 1) * (xe2[i] - xe[i]) + wa * (xe[i] - xe1[i]);
        factor = below == 0 ? 0 : static_cast<u32>((static_cast<u64>((ws[a] >> 1) * (xe[i] - xe1[i])) << 9) / below);
      }
      we[i] = by_factor(factor, 9, ws[a], ws[b]);
      re[i] = by_factor(factor, 9, colour_part(a, 0), colour_part(b, 0));
      ge[i] = by_factor(factor, 9, colour_part(a, 6), colour_part(b, 6));
      be[i] = by_factor(factor, 9, colour_part(a, 12), colour_part(b, 12));
      se[i] = by_factor(factor, 9, s_of(a), s_of(b)) - 0xFFFF;
      te[i] = by_factor(factor, 9, t_of(a), t_of(b)) - 0xFFFF;
    }
  }

  // A run of shadow masks starts with an empty stencil.
  const bool shadow = shape.mode == 3;
  const bool mask = shadow && shape.number == 0;
  if (mask) {
    if (!stencil_cleared_[static_cast<std::size_t>(line)]) {
      std::fill_n(stencil_.begin() + line * kWidth, kWidth, std::uint8_t{0});
      stencil_cleared_[static_cast<std::size_t>(line)] = true;
    }
  } else {
    stencil_cleared_[static_cast<std::size_t>(line)] = false;
  }

  // The span is drawn, and what is taken across it is measured, to one pixel
  // past the right edge.
  ++x4;
  u32 x1e = x1, x4e = x4;
  if (shape.alpha != 0 && (control_ & (kAntiAliasing | kEdgeMarking)) == 0) {
    if (hide_left) x1e = x2 + 1;
    if (hide_right) x4e = x3;
    if (!(hide_left && hide_right) && x4e <= x1e) x4e = x1e + 1;
  }

  // The top and the bottom line of a polygon are edges all the way across.
  const bool level = line == shape.top || line == shape.bottom - 1;

  int last_s = 0xFFFF, last_t = 0xFFFF;
  u32 texel_colour = 0;

  for (u32 x = x1; x < x4; ++x) {
    // An outline has no inside.
    if (!level && shape.alpha == 0 && x == x2 + 1 && x3 > x2) x = x3;
    if (x >= static_cast<u32>(kWidth)) break;

    int layer = 0;
    Pixel* front = &at(0, line, static_cast<int>(x));
    Pixel* back = &at(1, line, static_cast<int>(x));
    std::uint8_t& stencil = stencil_[static_cast<std::size_t>(line * kWidth) + x];

    // How far across the span, with perspective, in 256ths.
    constexpr u32 kNoPerspective = 0xFFFFFFFF;
    u32 factor;
    if (we[0] == we[1] && (we[0] & 0x7F) == 0) {
      factor = kNoPerspective;
    } else if (x <= x1) {
      factor = 0;
    } else {
      const u32 below = we[1] * (x4 - x) + we[0] * (x - x1);
      factor = below == 0 ? 0 : ((we[0] * (x - x1)) << 8) / below;
    }
    const auto across = [&](u32 from, u32 to) {
      return factor == kNoPerspective ? linear(from, to, x1, x, x4) : by_factor(factor, 8, from, to);
    };

    i32 depth;
    if (w_buffer_) {
      depth = static_cast<i32>(across(we[0], we[1]));
      if (shape.w_shift > 0)
        depth = static_cast<i32>(static_cast<u32>(depth) << shape.w_shift);
      else if (shape.w_shift < 0)
        depth >>= -shape.w_shift;
    } else {
      depth = static_cast<i32>(linear(ze[0], ze[1], x1, x, x4));
    }

    // Nearer than what is there; or, for polygons that ask for it, as near,
    // give or take a margin. The same against the back layer where the
    // front is an anti-aliased edge.
    const auto passes = [&](const Pixel& pixel) {
      if (!shape.depth_equal) return pixel.depth > depth;
      const std::int64_t margin = w_buffer_ ? 0xFF : 0x200;
      const std::int64_t difference = std::int64_t{pixel.depth} - depth;
      return difference >= -margin && difference <= margin;
    };
    const bool pass_front = passes(*front);
    const bool pass_back = anti_aliasing && front->edge && passes(*back);

    if (shadow) {
      if (mask) {
        // A mask is not drawn. It marks where it is hidden.
        if (!pass_front) stencil |= 1;
        if (!pass_back) stencil |= 2;
        continue;
      }
      // A shadow is drawn where a mask was hidden, and not on a pixel of its
      // own polygon number.
      if (!pass_front || (stencil & 1) == 0 || front->opaque_number == shape.number) {
        if (!pass_back || (stencil & 2) == 0 || back->opaque_number == shape.number) continue;
        layer = 1;
      }
    } else if (!pass_front) {
      if (!pass_back) continue;
      layer = 1;
    }
    Pixel& pixel = layer == 0 ? *front : *back;

    const u32 rv = across(re[0], re[1]) >> 3;
    const u32 gv = across(ge[0], ge[1]) >> 3;
    const u32 bv = across(be[0], be[1]) >> 3;
    u32 colour = (u32{shape.alpha != 0 ? shape.alpha : std::uint8_t{0x3F}} << 18) | (bv << 12) | (gv << 6) | rv;

    // One part of two colours multiplied, and of a toon table entry: the
    // table is looked up by the red part of the vertex colour.
    const auto times = [](u32 a, u32 b) { return ((a + 1) * (b + 1) - 1) / 64; };
    const auto toon = [&] { return six_bits_each(toon_table_[(colour & 0x3F) / 2]); };

    if (shape.format != 0) {
      // In sixteenths of a texel; the texel is the whole part.
      const int s16 = static_cast<i32>(across(se[0] + 0xFFFF, se[1] + 0xFFFF) - 0xFFFF);
      const int t16 = static_cast<i32>(across(te[0] + 0xFFFF, te[1] + 0xFFFF) - 0xFFFF);
      const int s = shape.smooth ? s16 : s16 >> 4;
      const int t = shape.smooth ? t16 : t16 >> 4;
      if (s != last_s || t != last_t) {
        last_s = s;
        last_t = t;
        texel_colour = shape.smooth ? smoothed_texel(shape, s16, t16) : texel(shape, s, t);
      }
      const u32 tex = texel_colour;
      u32 mixed = 0;
      switch (shape.mode) {
        case 0:  // Modulated: every part multiplied.
          for (unsigned shift = 0; shift < 24; shift += 6) mixed |= times(part_of(tex, shift), part_of(colour, shift)) << shift;
          break;
        case 1:
        case 3: {  // A decal: the texel over the vertex colour, by the texel's alpha.
          const u32 ta = alpha_of(tex);
          for (unsigned shift = 0; shift < 18; shift += 6)
            mixed |= ((part_of(tex, shift) * ta + part_of(colour, shift) * (63 - ta)) / 64) << shift;
          mixed |= colour & kAlphaMask;
          break;
        }
        default: {  // With the toon table.
          const u32 table = toon();
          for (unsigned shift = 0; shift < 18; shift += 6) {
            u32 part;
            if ((control_ & kHighlight) != 0)
              part = std::min<u32>(times(part_of(tex, shift), part_of(colour, shift)) + part_of(table, shift), 63);
            else
              part = times(part_of(tex, shift), part_of(table, shift));
            mixed |= part << shift;
          }
          mixed |= times(alpha_of(tex), alpha_of(colour)) << 18;
          break;
        }
      }
      colour = mixed;
    } else if (shape.mode == 2) {
      const u32 table = toon();
      u32 mixed = colour & kAlphaMask;
      for (unsigned shift = 0; shift < 18; shift += 6) {
        const u32 part = (control_ & kHighlight) != 0 ? std::min<u32>(part_of(colour, shift) + part_of(table, shift), 63)
                                                      : part_of(table, shift);
        mixed |= part << shift;
      }
      colour = mixed;
    }

    const u32 alpha = alpha_of(colour);
    // Nothing of it, or too little for the alpha test.
    if (alpha == 0) continue;
    if ((control_ & kAlphaTest) != 0 && (alpha >> 1) <= alpha_reference_) continue;
    // The pixels of a hidden edge, unless they are to be blended.
    if ((x < x1e || x >= x4e) && (alpha == 0x3F || (control_ & kAlphaBlending) == 0)) continue;

    if (alpha == 0x3F) {
      std::uint8_t edge_alpha = 0x3F;
      const bool edge = x <= x2 || x >= x3 || level;
      // What an anti-aliased edge covers goes to the back layer, to be mixed
      // with the edge when the line is done.
      if (anti_aliasing && layer == 0 && edge) {
        *back = *front;
        if (x <= x2)
          edge_alpha = static_cast<std::uint8_t>(linear(x1a, x2a, x1, x, x2));
        else if (x >= x3)
          edge_alpha = static_cast<std::uint8_t>(linear(x3a, x4a, x3, x, x4));
      }
      pixel.colour = colour;
      pixel.depth = depth;
      pixel.opaque_number = shape.number;
      pixel.translucent = false;
      pixel.fog = shape.fog;
      pixel.edge = edge;
      pixel.edge_alpha = edge_alpha;
    } else if (!pixel.translucent || pixel.translucent_number != shape.number) {
      // Translucent: over what is there, unless that is a translucent pixel
      // of the same polygon number.
      const auto blend = [&](Pixel& under) {
        under.colour = (control_ & kAlphaBlending) != 0 && alpha_of(under.colour) != 0
                           ? colour_between(under.colour, colour, 0, alpha, 63)
                           : colour;
        if (shape.new_depth) under.depth = depth;
        under.fog = under.fog && shape.fog;
        under.translucent = true;
        under.translucent_number = shape.number;
      };
      blend(pixel);
      if (anti_aliasing && layer == 0 && front->edge) blend(*back);
    }
  }
}

void PolygonRenderer::finish_line(int line) {
  const bool anti_aliasing = (control_ & kAntiAliasing) != 0;

  if ((control_ & kEdgeMarking) != 0 && edge_marking_ != EdgeMarking::Off) {
    // An edge pixel is given its polygon number's edge colour where a pixel
    // next to it has another polygon number and is further away. Past the
    // screen's edge is the clear plane.
    const std::uint8_t clear_number = static_cast<std::uint8_t>((clear_colour_ >> 24) & 0x3F);
    const i32 clear_depth = clear_depth_ == 0x7FFF ? 0xFFFFFF : static_cast<i32>(clear_depth_ << 9);
    const auto number_and_depth = [this](int x, int y) {
      const Pixel& p = at(0, y, x);
      return std::pair<std::uint8_t, i32>{p.opaque_number, p.depth};
    };
    // At the DS's resolution, one pixel is the width either way.
    const int width = edge_width(edge_marking_, 1);
    for (int x = 0; x < kWidth; ++x) {
      Pixel& pixel = at(0, line, x);
      if (!pixel.edge || !is_edge(number_and_depth, kWidth, kHeight, x, line, width, clear_number, clear_depth)) continue;
      pixel.colour = six_bits_each((0x1Fu << 15) | edge_colours_[pixel.opaque_number >> 3]);
      pixel.edge_alpha = 0x20;
    }
  }

  if ((control_ & kFog) != 0) {
    // Pixels that take fog are mixed with the fog colour by a density that
    // goes with their depth: 32 table entries, a step of depth apart, with
    // the values between them in proportion.
    const u32 fog = six_bits_each((((fog_colour_ >> 16) & 0x1F) << 15) | (fog_colour_ & 0x7FFF));
    const i32 step = 0x400 >> ((control_ >> kFogShift) & 0xF);
    for (int layer = 0; layer < (anti_aliasing ? 2 : 1); ++layer) {
      for (int x = 0; x < kWidth; ++x) {
        Pixel& pixel = at(layer, line, x);
        if (!pixel.fog) continue;
        const i32 offset = pixel.depth / 0x200 - static_cast<i32>(fog_offset_);
        const i32 n = step > 0 ? offset / step - 1 : (offset > 0 ? 31 : 0);
        u32 density;
        if (n >= 31) {
          density = fog_table_[31];
        } else if (n < 0 || step == 0) {
          density = fog_table_[0];
        } else {
          const i32 m = offset % step;
          density = m >= 0 ? static_cast<u32>((fog_table_[static_cast<std::size_t>(n) + 1] * m +
                                               fog_table_[static_cast<std::size_t>(n)] * (step - m)) /
                                              step)
                           : fog_table_[0];
        }
        if (density == 127) ++density;
        const auto mix = [&](unsigned shift) { return (part_of(fog, shift) * density + part_of(pixel.colour, shift) * (128 - density)) / 128; };
        if ((control_ & kFogAlphaOnly) != 0)
          pixel.colour = (pixel.colour & ~kAlphaMask) | (mix(18) << 18);
        else
          pixel.colour = mix(0) | (mix(6) << 6) | (mix(12) << 12) | (mix(18) << 18);
      }
    }
  }

  if (anti_aliasing) {
    // An edge that covers part of a pixel is mixed with what is behind it;
    // where nothing is, the coverage becomes its alpha.
    for (int x = 0; x < kWidth; ++x) {
      Pixel& pixel = at(0, line, x);
      if (pixel.edge_alpha >= 0x3F) continue;
      const Pixel& behind = at(1, line, x);
      if (alpha_of(behind.colour) != 0)
        pixel.colour = colour_between(behind.colour, pixel.colour, 0, pixel.edge_alpha, 0x3F);
      else
        pixel.colour = (pixel.colour & ~kAlphaMask) | (u32{pixel.edge_alpha} << 18);
    }
  }
}

std::uint32_t PolygonRenderer::smoothed_texel(const Shape& shape, int s16, int t16) const {
  // A texel's middle is half a texel into it: the four texels whose middles
  // are around the point, each weighted by how near it is, in 256ths.
  const int s = s16 - 8, t = t16 - 8;
  const int s0 = s >> 4, t0 = t >> 4;
  const u32 fs = static_cast<u32>(s & 15), ft = static_cast<u32>(t & 15);
  const u32 texels[4] = {texel(shape, s0, t0), texel(shape, s0 + 1, t0), texel(shape, s0, t0 + 1),
                         texel(shape, s0 + 1, t0 + 1)};
  const u32 weights[4] = {(16 - fs) * (16 - ft), fs * (16 - ft), (16 - fs) * ft, fs * ft};

  u32 parts[3] = {0, 0, 0};
  if (shape.format == 1 || shape.format == 6) {
    // An alpha of their own: mixed by weight and alpha, so that what is not
    // there does not darken what is.
    u32 alpha_sum = 0;  // Up to 256 * 63.
    for (int i = 0; i < 4; ++i) {
      const u32 a = weights[i] * alpha_of(texels[i]);
      alpha_sum += a;
      for (unsigned p = 0; p < 3; ++p) parts[p] += a * part_of(texels[i], p * 6);
    }
    if (alpha_sum == 0) return 0;
    u32 mixed = ((alpha_sum + 128) / 256) << 18;
    for (unsigned p = 0; p < 3; ++p) mixed |= ((parts[p] + alpha_sum / 2) / alpha_sum) << (p * 6);
    return mixed;
  }
  // There or not: drawn where at least half the weight is there, from what is.
  u32 covered = 0;
  for (int i = 0; i < 4; ++i) {
    if (alpha_of(texels[i]) == 0) continue;
    covered += weights[i];
    for (unsigned p = 0; p < 3; ++p) parts[p] += weights[i] * part_of(texels[i], p * 6);
  }
  if (covered * 2 < 256) return 0;
  u32 mixed = 0x3Fu << 18;
  for (unsigned p = 0; p < 3; ++p) mixed |= ((parts[p] + covered / 2) / covered) << (p * 6);
  return mixed;
}

std::uint32_t PolygonRenderer::texel(const Shape& shape, int s, int t) const {
  // Outside the texture: repeated (every other time mirrored, if asked) or
  // held at the edge.
  if (shape.repeat_s) {
    if (shape.flip_s && (s & shape.size_s) != 0) s = -1 - s;
    s &= shape.size_s - 1;
  } else {
    s = std::clamp(s, 0, shape.size_s - 1);
  }
  if (shape.repeat_t) {
    if (shape.flip_t && (t & shape.size_t_) != 0) t = -1 - t;
    t &= shape.size_t_ - 1;
  } else {
    t = std::clamp(t, 0, shape.size_t_ - 1);
  }

  const u32 at_texel = static_cast<u32>(t * shape.size_s + s);
  // A colour of the palette, 16 bits, or -1 where no bank is.
  const auto palette = [&](u32 base, u32 index) {
    const int low = video_->texture_palette(base + index * 2), high = video_->texture_palette(base + index * 2 + 1);
    return low < 0 || high < 0 ? -1 : low | high << 8;
  };
  const auto opaque = [](int colour) { return six_bits_each((0x1Fu << 15) | (static_cast<u32>(colour) & 0x7FFF)); };

  switch (shape.format) {
    case 1:    // Five bits of colour number, three of alpha.
    case 6: {  // Three bits of colour number, five of alpha.
      const int data = video_->texture(shape.texture_address + at_texel);
      if (data < 0) return 0;
      const u32 byte = static_cast<u32>(data);
      const u32 number = shape.format == 1 ? byte & 0x1F : byte & 0x07;
      const u32 raw = byte >> (shape.format == 1 ? 5 : 3);
      const u32 alpha = shape.format == 1 ? raw * 4 + raw / 2 : raw;
      const int colour = palette(shape.palette_address, number);
      if (colour < 0) return 0;
      return six_bits_each((alpha << 15) | (static_cast<u32>(colour) & 0x7FFF));
    }
    case 2:    // Four colours: two bits a texel.
    case 3:    // 16 colours: four bits.
    case 4: {  // 256 colours: eight bits.
      const unsigned bits = shape.format == 2 ? 2 : shape.format == 3 ? 4 : 8;
      const unsigned in_a_byte = 8 / bits;
      const int data = video_->texture(shape.texture_address + at_texel / in_a_byte);
      if (data < 0) return 0;
      const u32 number = (static_cast<u32>(data) >> ((static_cast<u32>(s) % in_a_byte) * bits)) & ((1u << bits) - 1);
      if (shape.transparent_0 && number == 0) return 0;
      const int colour = palette(shape.palette_address, number);
      return colour < 0 ? 0 : opaque(colour);
    }
    case 5: {
      // Blocks of four by four texels, two bits a texel, with four colours a
      // block. Which colours, and how the last two are made, is in a second
      // table, in the second texture slot.
      const u32 block = static_cast<u32>((t / 4) * (shape.size_s / 4) + s / 4);
      const int data = video_->texture(shape.texture_address + block * 4 + static_cast<u32>(t % 4));
      if (data < 0) return 0;
      const u32 number = (static_cast<u32>(data) >> ((s % 4) * 2)) & 3;
      const u32 table = 0x20000 + (shape.texture_address % 0x20000) / 2 + (shape.texture_address / 0x20000 == 2 ? 0x10000 : 0);
      const int low = video_->texture(table + block * 2), high = video_->texture(table + block * 2 + 1);
      if (low < 0 || high < 0) return 0;
      const u32 entry = static_cast<u32>(low | high << 8);
      const u32 base = shape.palette_address + (entry & 0x3FFF) * 4;
      const auto colour = [&](u32 index) {
        const int c = palette(base, index);
        return c < 0 ? 0 : opaque(c);
      };
      if (palette(base, number) < 0) return 0;
      switch (entry >> 14) {
        case 0: return number == 3 ? 0 : colour(number);
        case 1:
          if (number == 2) return colour_between(colour(0), colour(1), 0, 1, 2);
          return number == 3 ? 0 : colour(number);
        case 2: return colour(number);
        default:
          if (number == 2) return colour_between(colour(0), colour(1), 0, 3, 8);
          if (number == 3) return colour_between(colour(0), colour(1), 0, 5, 8);
          return colour(number);
      }
    }
    default: {  // Colours as such, 16 bits a texel; the top bit is the alpha.
      const u32 address = shape.texture_address + at_texel * 2;
      const int low = video_->texture(address), high = video_->texture(address + 1);
      if (low < 0 || high < 0) return 0;
      const u32 colour = static_cast<u32>(low | high << 8);
      return six_bits_each(((colour & 0x8000) != 0 ? 0x1Fu << 15 : 0u) | (colour & 0x7FFF));
    }
  }
}

}  // namespace bd::nds
