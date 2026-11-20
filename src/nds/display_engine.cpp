#include "nds/display_engine.h"

#include <algorithm>

namespace bd::nds {

namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWhite = 0x3FFFF;

// A 2D colour, five bits a part, made six: doubled.
u32 six_bits_each(u32 colour) { return ((colour & 0x1F) << 1) | ((colour & 0x3E0) << 2) | ((colour & 0x7C00) << 3); }

u32 part_of(u32 colour, unsigned shift) { return (colour >> shift) & 0x3F; }

// The sizes of bitmap backgrounds, across and down, by the two size bits.
constexpr int kBitmapSizes[4][2] = {{128, 128}, {256, 256}, {512, 256}, {512, 512}};

}  // namespace

DisplayEngine::DisplayEngine(Which which, const VideoMemory& video, const std::uint8_t* palette,
                             const std::uint8_t* sprites)
    : which_(which),
      video_(video),
      palette_(palette),
      sprites_(sprites),
      vram_(which == kMain ? 0x06000000 : 0x06200000),
      screen_(static_cast<std::size_t>(kWidth) * kHeight, kWhite),
      unlit_(screen_) {}

void DisplayEngine::attach(Arm9Io& io) {
  const u32 base = kBases[which_];
  const auto merge16 = [](std::uint16_t& into, u32 value, u32 mask) {
    into = static_cast<std::uint16_t>((into & ~mask) | (value & mask));
  };

  // The second engine has no 3D, no display from a bank and no choice of
  // where its tiles are.
  const u32 control_bits = which_ == kMain ? 0xFFFFFFFF : 0xC0B1FFF7;
  io.add(base + kControl, 4, [this] { return control_; }, [this, control_bits](u32 value, u32 mask) {
    mask &= control_bits;
    control_ = (control_ & ~mask) | (value & mask);
  });
  for (u32 n = 0; n < 4; ++n) {
    io.add(base + kBackgroundControl + 2 * n, 2, [this, n] { return u32{background_control_[n]}; },
           [this, n, merge16](u32 value, u32 mask) { merge16(background_control_[n], value, mask); });
    io.add(base + kScroll + 4 * n, 2, nullptr,
           [this, n, merge16](u32 value, u32 mask) { merge16(scroll_x_[n], value, mask & 0x01FF); });
    io.add(base + kScroll + 4 * n + 2, 2, nullptr,
           [this, n, merge16](u32 value, u32 mask) { merge16(scroll_y_[n], value, mask & 0x01FF); });
  }
  for (u32 n = 0; n < 2; ++n) {
    const u32 at = base + kMatrix + 0x10 * n;
    const auto number = [](std::int16_t& into) {
      return [&into](u32 value, u32 mask) {
        into = static_cast<std::int16_t>(static_cast<std::uint16_t>((static_cast<std::uint16_t>(into) & ~mask) | (value & mask)));
      };
    };
    io.add(at + 0, 2, nullptr, number(pa_[n]));
    io.add(at + 2, 2, nullptr, number(pb_[n]));
    io.add(at + 4, 2, nullptr, number(pc_[n]));
    io.add(at + 6, 2, nullptr, number(pd_[n]));
    // A reference point is 28 bits, signed; writing it also sets where the
    // current line is.
    const auto point = [](i32& written, i32& on_line) {
      return [&written, &on_line](u32 value, u32 mask) {
        mask &= 0x0FFFFFFF;
        u32 bits = (static_cast<u32>(written) & 0x0FFFFFFF & ~mask) | (value & mask);
        if ((bits & 0x08000000) != 0) bits |= 0xF0000000;
        written = on_line = static_cast<i32>(bits);
      };
    };
    io.add(at + 8, 4, nullptr, point(reference_x_[n], line_x_[n]));
    io.add(at + 12, 4, nullptr, point(reference_y_[n], line_y_[n]));
  }
  for (u32 n = 0; n < 2; ++n) {
    // The right (or lower) edge in the low byte, the left (or upper) in the
    // high.
    io.add(base + kWindowAcross + 2 * n, 2, nullptr, [this, n](u32 value, u32 mask) {
      if ((mask & 0x00FF) != 0) window_x2_[n] = static_cast<std::uint8_t>(value);
      if ((mask & 0xFF00) != 0) window_x1_[n] = static_cast<std::uint8_t>(value >> 8);
      window_flipped_[n] = window_x1_[n] > window_x2_[n];
    });
    io.add(base + kWindowAcross + 4 + 2 * n, 2, nullptr, [this, n](u32 value, u32 mask) {
      if ((mask & 0x00FF) != 0) window_y2_[n] = static_cast<std::uint8_t>(value);
      if ((mask & 0xFF00) != 0) window_y1_[n] = static_cast<std::uint8_t>(value >> 8);
    });
  }
  io.add(base + kWindowInside, 2, [this] { return u32{window_inside_}; },
         [this, merge16](u32 value, u32 mask) { merge16(window_inside_, value, mask & 0x3F3F); });
  io.add(base + kWindowOutside, 2, [this] { return u32{window_outside_}; },
         [this, merge16](u32 value, u32 mask) { merge16(window_outside_, value, mask & 0x3F3F); });
  io.add(base + kMosaic, 2, nullptr, [this, merge16](u32 value, u32 mask) { merge16(mosaic_, value, mask); });
  io.add(base + kBlendControl, 2, [this] { return u32{blend_control_}; },
         [this, merge16](u32 value, u32 mask) { merge16(blend_control_, value, mask & 0x3FFF); });
  io.add(base + kBlendAlpha, 2, [this] { return u32{blend_alpha_}; },
         [this, merge16](u32 value, u32 mask) { merge16(blend_alpha_, value, mask & 0x1F1F); });
  io.add(base + kBlendBrightness, 2, nullptr, [this](u32 value, u32 mask) {
    if ((mask & 0xFF) != 0) blend_brightness_ = static_cast<std::uint8_t>(std::min<u32>(value & 0x1F, 16));
  });
  io.add(base + kMasterBrightness, 2, [this] { return u32{master_brightness_}; },
         [this, merge16](u32 value, u32 mask) { merge16(master_brightness_, value, mask & 0xC01F); });
}

void DisplayEngine::start_frame() {
  line_x_ = reference_x_;
  line_y_ = reference_y_;
}

void DisplayEngine::start_line(int line) {
  for (unsigned n = 0; n < 2; ++n) {
    if (line == window_y2_[n])
      window_on_line_[n] = false;
    else if (line == window_y1_[n])
      window_on_line_[n] = true;
  }
}

std::uint32_t DisplayEngine::colour_of(unsigned index) const {
  return six_bits_each(static_cast<u32>(palette_[index * 2] | palette_[index * 2 + 1] << 8));
}

unsigned DisplayEngine::shown_inside(int x) const {
  if ((control_ & (kWindow0 | kWindow1 | kSpriteWindow)) == 0) return 0x3F;
  for (unsigned n = 0; n < 2; ++n) {
    if ((control_ & (kWindow0 << n)) == 0 || !window_on_line_[n]) continue;
    const int left = std::min(window_x1_[n], window_x2_[n]), right = std::max(window_x1_[n], window_x2_[n]);
    if ((x >= left && x < right) != window_flipped_[n]) return (window_inside_ >> (8 * n)) & 0x3Fu;
  }
  if ((control_ & kSpriteWindow) != 0 && sprite_window_[static_cast<std::size_t>(x)]) return (window_outside_ >> 8) & 0x3Fu;
  return window_outside_ & 0x3Fu;
}

void DisplayEngine::place(unsigned background, int x, std::uint32_t colour, bool from_3d, std::uint8_t alpha) {
  if ((shown_inside(x) & (1u << background)) == 0) return;
  Dot dot;
  dot.colour = colour;
  dot.priority = static_cast<std::int8_t>(background_control_[background] & 3);
  dot.layer = static_cast<std::uint8_t>(background);
  dot.from_3d = from_3d;
  dot.alpha = alpha;
  // The two topmost layers are kept. Of equal priorities the one placed
  // later is on top, and backgrounds are placed from 3 down to 0.
  Dot& top = top_[static_cast<std::size_t>(x)];
  Dot& below = below_[static_cast<std::size_t>(x)];
  if (dot.priority <= top.priority) {
    below = top;
    top = dot;
  } else if (dot.priority <= below.priority) {
    below = dot;
  }
}

void DisplayEngine::draw_line(int line) {
  marked_ = false;
  Dot backdrop;
  backdrop.colour = colour_of(0);
  top_.fill(backdrop);
  below_.fill(backdrop);
  sprite_window_.fill(false);
  // Sprites first: those that are a window, then those that are drawn.
  if ((control_ & kShowSprites) != 0) {
    if ((control_ & kSpriteWindow) != 0) draw_sprites(line, true);
    draw_sprites(line, false);
  }

  // Which kind each background is goes by the mode; they are placed from the
  // last to the first.
  const auto shown = [&](unsigned background) { return (control_ & (kShowBackground0 << background)) != 0; };
  const unsigned mode = control_ & 7;
  if (mode == 6) {
    if (shown(2)) draw_large(2, line);
  } else if (mode == 7) {
    ++notes_["background mode 7, which is none"];
  } else {
    if (shown(3)) {
      if (mode == 0)
        draw_tiles(3, line);
      else if (mode <= 2)
        draw_matrix_tiles(3, line);
      else
        draw_extended(3, line);
    }
    if (shown(2)) {
      if (mode == 0 || mode == 1 || mode == 3)
        draw_tiles(2, line);
      else if (mode == 2 || mode == 4)
        draw_matrix_tiles(2, line);
      else
        draw_extended(2, line);
    }
    if (shown(1)) draw_tiles(1, line);
    if (shown(0)) {
      if (which_ == kMain && (control_ & kBackground0Is3D) != 0)
        hardware_3d_ ? mark_3d() : draw_3d(line);
      else
        draw_tiles(0, line);
    }
  }

  blend(line);
  for (std::size_t x = 0; x < static_cast<std::size_t>(kWidth); ++x) mixed_[x] = top_[x].colour;

  u32* shown_line = &screen_[static_cast<std::size_t>(line) * kWidth];
  switch ((control_ >> kDisplayModeShift) & 3) {
    case 0:  // Off: white.
      std::fill_n(shown_line, kWidth, kWhite);
      break;
    case 1:
      std::copy(mixed_.begin(), mixed_.end(), shown_line);
      break;
    case 2: {
      // A bank of video memory as a picture of 16 bits a pixel. The bank has
      // to be placed as plain memory.
      const u32 from = 0x06800000 + ((control_ >> 1) & 0x60000) + static_cast<u32>(line) * kWidth * 2;
      for (u32 x = 0; x < static_cast<u32>(kWidth); ++x) shown_line[x] = six_bits_each(read(from + x * 2, 2));
      break;
    }
    default:
      ++notes_["the display fed from main memory"];
      std::fill_n(shown_line, kWidth, u32{0});
      break;
  }

  std::copy_n(shown_line, kWidth, &unlit_[static_cast<std::size_t>(line) * kWidth]);
  // The master brightness: every part a number of sixteenths of the way to
  // white (1) or to black (2).
  const unsigned direction = master_brightness_ >> 14;
  const u32 factor = std::min<u32>(master_brightness_ & 0x1F, 16);
  if ((direction == 1 || direction == 2) && factor != 0) {
    for (int x = 0; x < kWidth; ++x) {
      u32 result = 0;
      for (unsigned shift = 0; shift < 18; shift += 6) {
        const u32 part = part_of(shown_line[x], shift);
        result |= (direction == 1 ? part + (63 - part) * factor / 16 : part - part * factor / 16) << shift;
      }
      shown_line[x] = result;
    }
  }
  if (marked_ && ((control_ >> kDisplayModeShift) & 3) == 1)
    for (std::size_t x = 0; x < static_cast<std::size_t>(kWidth); ++x) shown_line[x] |= marks_[x];
}

void DisplayEngine::draw_sprites(int line, bool window_pass) {
  const u32 memory = which_ == kMain ? 0x06400000 : 0x06600000;
  const bool second = which_ == kSub;
  // Width and height by shape (square, wide, tall) and size.
  static constexpr std::uint8_t kSizes[3][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                                   {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                                   {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
  const auto word = [&](u32 at) { return static_cast<u32>(sprites_[at] | sprites_[at + 1] << 8); };

  for (u32 i = 0; i < 128; ++i) {
    // Three words of attributes a sprite.
    const u32 a0 = word(i * 8), a1 = word(i * 8 + 2), a2 = word(i * 8 + 4);
    // Normal, translucent, a window, or a bitmap. Switched off is "no
    // matrix" with the bit that otherwise doubles the size.
    const unsigned kind = (a0 >> 10) & 3;
    if (((a0 >> 8) & 3) == 2 || (kind == 2) != window_pass) continue;
    const unsigned shape = a0 >> 14;
    if (shape == 3) continue;  // No such shape.
    const int width = kSizes[shape][a1 >> 14][0], height = kSizes[shape][a1 >> 14][1];
    const bool matrix = (a0 & 0x100) != 0;
    // With a matrix the sprite can be given twice the room to be drawn in.
    const bool doubled = (a0 & 0x300) == 0x300;
    const int room_x = doubled ? width * 2 : width, room_y = doubled ? height * 2 : height;

    int y = static_cast<int>(a0 & 0xFF);
    if (y >= kHeight) y -= 256;
    const int mosaic_down = ((mosaic_ >> 12) & 0xF) + 1;
    const int in_y = ((a0 & (1u << 12)) != 0 ? line - line % mosaic_down : line) - y;
    if (in_y < 0 || in_y >= room_y) continue;
    int x = static_cast<int>(a1 & 0x1FF);
    if (x >= kWidth) x -= 512;
    const std::int8_t priority = static_cast<std::int8_t>(static_cast<int>((a2 >> 10) & 3) - 1);

    // The matrix: one of 32 sets of four numbers, kept in the gaps between
    // the sprites' attributes.
    const u32 set = ((a1 >> 9) & 0x1F) * 0x20;
    const auto number = [&](u32 at) { return static_cast<int>(static_cast<std::int16_t>(word(set + at))); };
    const int pa = number(0x06), pb = number(0x0E), pc = number(0x16), pd = number(0x1E);

    const bool colours_256 = (a0 & (1u << 13)) != 0;
    const bool extended = colours_256 && (control_ & (1u << 31)) != 0;
    // With extended palettes on and no bank for them, the sprite is not drawn.
    if (kind != 3 && extended && video_.extended_palette(second, 4, 0) < 0) continue;

    // Where the sprite's pixels are. Tiles: numbered in steps of 32 bytes
    // and laid out as a sheet 32 tiles wide, or (one-dimensional) in steps
    // the control register sets, one after the other. Bitmaps likewise.
    const bool one_dimensional = (control_ & (1u << 4)) != 0;
    const u32 tiles = memory + (a2 & 0x3FF) * (one_dimensional ? 32u << ((control_ >> 20) & 3) : 32u);
    const int sheet = one_dimensional ? width : (colours_256 ? 128 : 256);
    u32 bitmap;
    int bitmap_width;
    if ((control_ & (1u << 6)) != 0) {
      bitmap = memory + (a2 & 0x3FF) * ((control_ & (1u << 22)) != 0 ? 256u : 128u);
      bitmap_width = width;
    } else {
      const u32 across = (control_ & (1u << 5)) != 0 ? 0x1F : 0x0F;
      bitmap = memory + (a2 & across) * 0x10 + (a2 & 0x3FF & ~across) * 0x80;
      bitmap_width = (control_ & (1u << 5)) != 0 ? 256 : 128;
    }

    for (int j = 0; j < room_x; ++j) {
      // Which pixel of the sprite is at pixel `j` of its room.
      int at_x, px, py;
      if (matrix) {
        at_x = x + j;
        px = ((pa * (j - room_x / 2) + pb * (in_y - room_y / 2)) >> 8) + width / 2;
        py = ((pc * (j - room_x / 2) + pd * (in_y - room_y / 2)) >> 8) + height / 2;
        if (px < 0 || px >= width || py < 0 || py >= height) continue;
      } else {
        at_x = (a1 & (1u << 12)) != 0 ? x + width - j - 1 : x + j;
        px = j;
        py = (a1 & (1u << 13)) != 0 ? height - 1 - in_y : in_y;
      }
      if (at_x < 0 || at_x >= kWidth) continue;

      u32 colour;
      if (kind == 3) {
        // 16 bits a pixel; the top bit says whether there is one.
        const u32 pixel = read(bitmap + static_cast<u32>(py * bitmap_width + px) * 2, 2);
        if ((pixel & 0x8000) == 0) continue;
        colour = six_bits_each(pixel);
      } else if (colours_256) {
        const u32 n = read(tiles + static_cast<u32>(((py / 8) * sheet + py % 8) * 8 + (px / 8) * 64 + px % 8), 1);
        if (n == 0) continue;
        if (extended) {
          // Sixteen palettes of 256 colours.
          const u32 at = ((a2 & 0xF000) >> 3) + n * 2;
          colour = six_bits_each(static_cast<u32>(video_.extended_palette(second, 4, at) |
                                                   video_.extended_palette(second, 4, at + 1) << 8));
        } else {
          colour = colour_of(256 + n);
        }
      } else {
        const u32 byte = read(tiles + static_cast<u32>(((py / 8) * sheet + py % 8) * 4 + (px / 8) * 32 + (px % 8) / 2), 1);
        const u32 n = (px & 1) != 0 ? byte >> 4 : byte & 0xF;
        if (n == 0) continue;
        // Sixteen palettes of 16 colours.
        colour = colour_of(256 + ((a2 >> 12) & 0xF) * 16 + n);
      }

      const std::size_t at = static_cast<std::size_t>(at_x);
      if (kind == 2) {
        sprite_window_[at] = true;
        continue;
      }
      if ((shown_inside(at_x) & 0x10) == 0) continue;
      // Sprites are one layer: a later one replaces an earlier one only with
      // a higher priority.
      Dot& top = top_[at];
      if (top.layer == 5 || priority < top.priority) {
        top.colour = colour;
        top.priority = priority;
        top.layer = 4;
        top.translucent = kind == 1;
      }
    }
  }
}

void DisplayEngine::draw_3d(int line) {
  if (picture_3d_ == nullptr || picture_3d_->size() < static_cast<std::size_t>(kWidth) * kHeight) return;
  const u32* pixels = &(*picture_3d_)[static_cast<std::size_t>(line) * kWidth];
  for (int x = 0; x < kWidth; ++x) {
    const std::uint8_t alpha = static_cast<std::uint8_t>(part_of(pixels[x], 18));
    if (alpha != 0) place(0, x, pixels[x] & 0x3FFFF, true, alpha);
  }
}

void DisplayEngine::mark_3d() {
  // Where the 3D layer would be on top, and how a pixel of it would be mixed
  // with what is then below it, as blend() does.
  const unsigned effect = (blend_control_ >> 6) & 3;
  const auto priority = static_cast<std::int8_t>(background_control_[0] & 3);
  marks_.fill(0);
  for (int x = 0; x < kWidth; ++x) {
    const unsigned inside = shown_inside(x);
    const Dot& below = top_[static_cast<std::size_t>(x)];
    if ((inside & 1) == 0 || priority > below.priority) continue;
    u32 mark = kMark3D;
    if ((blend_control_ & (1u << (8 + below.layer))) != 0)
      mark |= 1u << kMarkEffectShift;
    else if (effect >= 2 && (blend_control_ & 1) != 0 && (inside & 0x20) != 0)
      mark |= effect << kMarkEffectShift | u32{blend_brightness_} << kMarkFactorShift;
    marks_[static_cast<std::size_t>(x)] = mark;
    marked_ = true;
  }
}

void DisplayEngine::draw_tiles(unsigned background, int line) {
  const u32 bg_control = background_control_[background];
  // The map of tile numbers and the tiles' pixels, each at a place the
  // background's control register names (the main engine's control register
  // can move both on in steps of 64 KiB).
  u32 map = vram_ + ((control_ >> 11) & 0x70000) + ((bg_control << 3) & 0x0F800);
  const u32 pixels = vram_ + ((control_ >> 8) & 0x70000) + ((bg_control << 12) & 0x3C000);

  // The line of the background that is on this line of the screen. A map is
  // 32 tiles across; a background 512 high has a second map after the first
  // (after the first two, if it is 512 wide too).
  const int mosaic_down = ((mosaic_ >> 4) & 0xF) + 1;
  const int from_line = (bg_control & (1u << 6)) != 0 ? line - line % mosaic_down : line;
  const u32 y = (static_cast<u32>(from_line) + scroll_y_[background]) & 0x1FF;
  map += (y & 0xF8) << 3;
  if (y >= 256 && (bg_control & (1u << 15)) != 0) map += (bg_control & (1u << 14)) != 0 ? 0x1000 : 0x800;

  const bool colours_256 = (bg_control & (1u << 7)) != 0;
  // With 256 colours a tile can use an extended palette: sixteen palettes of
  // 256 colours in the slot of its background (or, for backgrounds 0 and 1,
  // two slots on, if the background asks).
  const unsigned slot = background + ((background < 2 && (bg_control & (1u << 13)) != 0) ? 2 : 0);
  const bool second = which_ == kSub;
  const bool extended =
      colours_256 && (control_ & kExtendedPalettes) != 0 && video_.extended_palette(second, slot, 0) >= 0;

  const int width = kWidth + (scroll_x_[background] & 7);
  for (int i = 0; i < width; i += 8) {
    const u32 x_in = (static_cast<u32>(i) + scroll_x_[background]) & 0x1FF;
    u32 entry_at = map + ((x_in & 0xF8) >> 2);
    // A background 512 wide has a second map to the right of the first.
    if (x_in >= 256 && (bg_control & (1u << 14)) != 0) entry_at += 0x800;
    // A map entry: the tile's number, whether it is mirrored across and
    // upside down, and its palette.
    const u32 entry = read(entry_at, 2);
    const u32 row = (entry & (1u << 11)) != 0 ? 7 - (y & 7) : (y & 7);
    const bool mirrored = (entry & (1u << 10)) != 0;
    const unsigned bits = colours_256 ? 8 : 4;
    const u32 row_at = pixels + (entry & 0x3FF) * (colours_256 ? 64 : 32) + row * bits;
    u64 numbers = read(row_at, 4);
    if (colours_256) numbers |= static_cast<u64>(read(row_at + 4, 4)) << 32;
    int x = i - static_cast<int>(x_in & 7) + (mirrored ? 7 : 0);
    for (; numbers != 0; numbers >>= bits, x += mirrored ? -1 : 1) {
      const u32 number = static_cast<u32>(numbers) & ((1u << bits) - 1);
      // Colour 0 is see-through.
      if (number == 0 || x < 0 || x >= kWidth) continue;
      u32 colour;
      if (extended) {
        const u32 at = ((entry >> 3) & 0x1E00) + number * 2;
        colour = six_bits_each(static_cast<u32>(video_.extended_palette(second, slot, at) |
                                                 video_.extended_palette(second, slot, at + 1) << 8));
      } else if (colours_256) {
        colour = colour_of(number);
      } else {
        colour = colour_of(((entry >> 12) & 0xF) * 16 + number);
      }
      place(background, x, colour);
    }
  }
}

void DisplayEngine::draw_matrix_tiles(unsigned background, int line) {
  (void)line;
  const unsigned n = background - 2;
  const u32 bg_control = background_control_[background];
  const u32 map = vram_ + ((bg_control << 3) & 0x0F800) + ((control_ >> 11) & 0x70000);
  const u32 pixels = vram_ + ((bg_control << 12) & 0x3C000) + ((control_ >> 8) & 0x70000);
  // Square, 128 to 1024 pixels; a map entry is a byte, a tile 256 colours.
  const int size = 128 << ((bg_control >> 14) & 3);
  i32 at_x = line_x_[n] - pa_[n], at_y = line_y_[n] - pc_[n];
  for (int i = 0; i < kWidth; ++i) {
    int x = (at_x += pa_[n]) >> 8;
    int y = (at_y += pc_[n]) >> 8;
    // Outside the background: repeated, or nothing.
    if ((bg_control & (1u << 13)) != 0) {
      x &= size - 1;
      y &= size - 1;
    } else if (x < 0 || x >= size || y < 0 || y >= size) {
      continue;
    }
    const u32 tile = read(map + static_cast<u32>((y / 8) * (size / 8) + x / 8), 1);
    const u32 number = read(pixels + tile * 64 + static_cast<u32>((y & 7) * 8 + (x & 7)), 1);
    if (number != 0) place(background, i, colour_of(number));
  }
  // The next line starts one step down the matrix.
  line_x_[n] += pb_[n];
  line_y_[n] += pd_[n];
}

void DisplayEngine::draw_extended(unsigned background, int line) {
  (void)line;
  const unsigned n = background - 2;
  const u32 bg_control = background_control_[background];
  i32 at_x = line_x_[n] - pa_[n], at_y = line_y_[n] - pc_[n];
  const bool repeated = (bg_control & (1u << 13)) != 0;
  // Where on the background pixel `i` of the line is; false if nowhere.
  const auto next = [&](int size_x, int size_y, int& x, int& y) {
    x = (at_x += pa_[n]) >> 8;
    y = (at_y += pc_[n]) >> 8;
    if (repeated) {
      x &= size_x - 1;
      y &= size_y - 1;
      return true;
    }
    return x >= 0 && x < size_x && y >= 0 && y < size_y;
  };

  if ((bg_control & (1u << 7)) != 0) {
    // A bitmap: 16 bits a pixel with the colour as such, or a byte a pixel
    // with a palette.
    const u32 data = vram_ + ((bg_control << 6) & 0x7C000);
    const int size_x = kBitmapSizes[(bg_control >> 14) & 3][0], size_y = kBitmapSizes[(bg_control >> 14) & 3][1];
    const bool direct = (bg_control & (1u << 2)) != 0;
    for (int i = 0; i < kWidth; ++i) {
      int x, y;
      if (!next(size_x, size_y, x, y)) continue;
      if (direct) {
        // The top bit says whether there is a pixel.
        const u32 pixel = read(data + static_cast<u32>(y * size_x + x) * 2, 2);
        if ((pixel & 0x8000) != 0) place(background, i, six_bits_each(pixel));
      } else {
        const u32 number = read(data + static_cast<u32>(y * size_x + x), 1);
        if (number != 0) place(background, i, colour_of(number));
      }
    }
  } else {
    // Tiles through a matrix, with map entries of 16 bits as tiled
    // backgrounds have: mirroring, and palettes if extended palettes are on.
    const u32 map = vram_ + ((bg_control << 3) & 0x0F800) + ((control_ >> 11) & 0x70000);
    const u32 pixels = vram_ + ((bg_control << 12) & 0x3C000) + ((control_ >> 8) & 0x70000);
    const int size = 128 << ((bg_control >> 14) & 3);
    const bool second = which_ == kSub;
    const bool extended = (control_ & kExtendedPalettes) != 0;
    const bool slot_there = video_.extended_palette(second, background, 0) >= 0;
    for (int i = 0; i < kWidth; ++i) {
      int x, y;
      if (!next(size, size, x, y)) continue;
      const u32 entry = read(map + static_cast<u32>((y / 8) * (size / 8) + x / 8) * 2, 2);
      if (extended && !slot_there) continue;
      const int row = ((entry & (1u << 11)) != 0 ? 7 - y : y) & 7;
      const int column = ((entry & (1u << 10)) != 0 ? 7 - x : x) & 7;
      const u32 number = read(pixels + (entry & 0x3FF) * 64 + static_cast<u32>(row * 8 + column), 1);
      if (number == 0) continue;
      if (extended) {
        const u32 at = ((entry >> 3) & 0x1E00) + number * 2;
        place(background, i,
              six_bits_each(static_cast<u32>(video_.extended_palette(second, background, at) |
                                             video_.extended_palette(second, background, at + 1) << 8)));
      } else {
        place(background, i, colour_of(number));
      }
    }
  }
  line_x_[n] += pb_[n];
  line_y_[n] += pd_[n];
}

void DisplayEngine::draw_large(unsigned background, int line) {
  (void)line;
  const unsigned n = background - 2;
  const u32 bg_control = background_control_[background];
  // One bitmap of a byte a pixel that takes all the background memory: 512
  // by 1024, or 1024 by 512.
  const int size_x = ((bg_control >> 14) & 3) != 0 ? 1024 : 512;
  const int size_y = ((bg_control >> 14) & 3) != 0 ? 512 : 1024;
  i32 at_x = line_x_[n] - pa_[n], at_y = line_y_[n] - pc_[n];
  for (int i = 0; i < kWidth; ++i) {
    int x = (at_x += pa_[n]) >> 8;
    int y = (at_y += pc_[n]) >> 8;
    if ((bg_control & (1u << 13)) != 0) {
      x &= size_x - 1;
      y &= size_y - 1;
    } else if (x < 0 || x >= size_x || y < 0 || y >= size_y) {
      continue;
    }
    // The second engine has a quarter of the memory: the bitmap repeats.
    if (which_ == kSub) y &= size_y / 4 - 1;
    const u32 number = read(vram_ + static_cast<u32>(y * size_x + x), 1);
    if (number != 0) place(background, i, colour_of(number));
  }
  line_x_[n] += pb_[n];
  line_y_[n] += pd_[n];
}

void DisplayEngine::blend(int line) {
  (void)line;
  // The blend control: which layers can be the top one of an effect (bits 0
  // to 5), which effect (bits 6 and 7), which layers can be the one below
  // (bits 8 to 13).
  const unsigned effect = (blend_control_ >> 6) & 3;
  const auto first = [&](const Dot& dot) { return (blend_control_ & (1u << dot.layer)) != 0; };
  const auto second = [&](const Dot& dot) { return (blend_control_ & (1u << (8 + dot.layer))) != 0; };
  const auto mix = [](u32 a, u32 weight_a, u32 b, u32 weight_b, u32 over) {
    u32 result = 0;
    for (unsigned shift = 0; shift < 18; shift += 6)
      result |= std::min<u32>((part_of(a, shift) * weight_a + part_of(b, shift) * weight_b) / over, 63) << shift;
    return result;
  };

  for (int x = 0; x < kWidth; ++x) {
    Dot& top = top_[static_cast<std::size_t>(x)];
    const Dot& below = below_[static_cast<std::size_t>(x)];
    bool alpha = false;
    if (top.from_3d) {
      if (second(below)) {
        // A 3D pixel over a layer that can be blended with: by the 3D
        // pixel's own alpha, whatever the effect chosen. A solid one stays.
        const u32 weight = u32{top.alpha} + 1;
        if (weight != 64) top.colour = mix(top.colour, weight, below.colour, 64 - weight, 64);
        continue;
      }
      if (effect < 2 || !first(top)) continue;
    } else if (top.translucent) {
      // A sprite that asks to be blended is, if what is below allows it.
      if (second(below))
        alpha = true;
      else if (effect < 2 || !first(top))
        continue;
    } else if (effect == 0 || !first(top) || (effect == 1 && !second(below))) {
      continue;
    }

    // A window can switch effects off inside it.
    if (!alpha && (shown_inside(x) & 0x20) == 0) continue;

    if (alpha || effect == 1) {
      // Sixteenths of the top layer plus sixteenths of the one below.
      const u32 weight_top = std::min<u32>(blend_alpha_ & 0x1F, 16), weight_below = std::min<u32>((blend_alpha_ >> 8) & 0x1F, 16);
      top.colour = mix(top.colour, weight_top, below.colour, weight_below, 16);
    } else if (blend_brightness_ != 0) {
      u32 result = 0;
      for (unsigned shift = 0; shift < 18; shift += 6) {
        const u32 part = part_of(top.colour, shift);
        result |= (effect == 2 ? part + (63 - part) * blend_brightness_ / 16 : part - part * blend_brightness_ / 16) << shift;
      }
      top.colour = result;
    }
  }
}

}  // namespace bd::nds
