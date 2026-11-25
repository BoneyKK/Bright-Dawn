// A 2D display engine's picture (nds/frame_2d.h): each pixel as
// nds::DisplayEngine::draw_line() makes it, step by step, from the line's
// registers, its copies of palette and sprite attribute memory, and video
// memory. 256 by 192; colours of six bits a part as eight (the top two bits
// again at the bottom), and the mark for the 3D scene (bits 24 to 31 of
// DisplayEngine's colours) as the alpha.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_colour;

// All nine banks one after another (nds::VideoMemory::kBankStarts).
layout(std430, set = 2, binding = 0) readonly buffer Video { uint video[]; };
// nds::VideoMemory::layout_2d(): where the engines' pages are in `video`.
layout(std430, set = 2, binding = 1) readonly buffer Layout { uint layout_2d[]; };
// The engine's lines: nds::Frame2D::kWords words each.
layout(std430, set = 2, binding = 2) readonly buffer Lines { uint lines[]; };
// Copies of the engine's palette and sprite attribute memory, 256 words each.
layout(std430, set = 2, binding = 3) readonly buffer Palettes { uint palettes[]; };
layout(std430, set = 2, binding = 4) readonly buffer Sprites { uint sprites[]; };
// nds::Video2D::changes, then nds::Video2D::changed: what changed in `video`
// while the lines were drawn.
layout(std430, set = 2, binding = 5) readonly buffer Changes { uint changes[]; };
layout(set = 3, binding = 0) uniform Engine {
  // x: 0 the main engine, 1 the second. y: 1 to leave the marks out. z: how
  // many changes there are.
  uvec4 u_engine;
};

const int kWidth = 256, kHeight = 192;
const uint kWhite = 0x3FFFFu;
const uint kMark3D = 1u << 31;

// The line's registers.
uint w[24];
uint palette_at, sprites_at;
bool second_engine;
int line;

// --- Video memory, as nds::VideoMemory::peek() reads it.

uint layout_entry(uint address) {
  switch (address & 0x00E00000u) {
    case 0x00000000u: return layout_2d[(address & 0x7FFFFu) >> 14];
    case 0x00200000u: return layout_2d[32u + ((address & 0x1FFFFu) >> 14)];
    case 0x00400000u: return layout_2d[40u + ((address & 0x3FFFFu) >> 14)];
    case 0x00600000u: return layout_2d[56u + ((address & 0x1FFFFu) >> 14)];
    default: return layout_2d[64u + ((address & 0xFFFFFu) >> 14)];
  }
}

// The word of all the banks with byte `at` in it, as this line reads it: in
// `video`, or in the last change to its block seen on the line.
const uint kBlockSize = 256u, kBlocks = 0xA4000u / kBlockSize;
uint word_of(uint at) {
  const uint count = u_engine.z;
  if (count != 0u) {
    const uint block = at / kBlockSize, first = changes[2u * block], n = changes[2u * block + 1u];
    // The first of the block's changes seen from a later line.
    uint low = first, high = first + n;
    while (low < high) {
      const uint middle = (low + high) / 2u;
      if ((changes[2u * kBlocks + middle] & 0xFFu) <= uint(line)) low = middle + 1u;
      else high = middle;
    }
    if (low != first) {
      const uint which = changes[2u * kBlocks + low - 1u] >> 8;
      return changes[2u * kBlocks + count + which * (kBlockSize / 4u) + (at % kBlockSize) / 4u];
    }
  }
  return video[at >> 2];
}

uint byte_in_all(uint at) { return (word_of(at) >> ((at & 3u) * 8u)) & 0xFFu; }

uint read8(uint address) {
  uint e = layout_entry(address);
  return e == 0u ? 0u : byte_in_all(((e - 1u) << 13) + (address & 0x3FFFu));
}

// Aligned to its size, as the engines read it: in one page.
uint read16(uint address) {
  uint e = layout_entry(address);
  if (e == 0u) return 0u;
  uint at = ((e - 1u) << 13) + (address & 0x3FFFu);
  return (word_of(at) >> ((at & 2u) * 8u)) & 0xFFFFu;
}

// A byte of an extended palette (slot 0 to 3 backgrounds, 4 sprites), or -1
// where no bank is.
int extended_palette(uint slot, uint offset) {
  uint e = layout_2d[128u + (second_engine ? 5u : 0u) + slot];
  if (e == 0u) return -1;
  return int(byte_in_all(((e - 1u) << 13) + (offset & 0x1FFFu)));
}

uint extended_colour(uint slot, uint at) {
  return uint(extended_palette(slot, at)) | uint(extended_palette(slot, at + 1u)) << 8;
}

// --- Colours.

uint six_bits_each(uint c) { return ((c & 0x1Fu) << 1) | ((c & 0x3E0u) << 2) | ((c & 0x7C00u) << 3); }
uint part_of(uint c, uint shift) { return (c >> shift) & 0x3Fu; }

// 0 to 255 the backgrounds' palette, 256 to 511 the sprites'.
uint colour_of(uint index) {
  uint word = palettes[palette_at + (index >> 1)];
  return six_bits_each((word >> ((index & 1u) * 16u)) & 0xFFFFu);
}

uint sprite_word(uint at) {  // 16 bits at byte `at` of sprite attribute memory.
  return (sprites[sprites_at + (at >> 2)] >> ((at & 2u) * 8u)) & 0xFFFFu;
}

// --- The registers.

uint control() { return w[0]; }
uint background_control(uint n) { return (w[1 + n / 2u] >> ((n & 1u) * 16u)) & 0xFFFFu; }
uint scroll_x(uint n) { return w[3 + n] & 0xFFFFu; }
uint scroll_y(uint n) { return w[3 + n] >> 16; }
int signed16(uint v) { return int(v << 16) >> 16; }
int pa(uint n) { return signed16(w[7 + 2 * n]); }
int pb(uint n) { return signed16(w[7 + 2 * n] >> 16); }
int pc(uint n) { return signed16(w[8 + 2 * n]); }
int pd(uint n) { return signed16(w[8 + 2 * n] >> 16); }
int line_x(uint n) { return int(w[11 + 2 * n]); }
int line_y(uint n) { return int(w[12 + 2 * n]); }
uint window_inside() { return w[17] & 0xFFFFu; }
uint window_outside() { return w[17] >> 16; }
uint mosaic() { return w[18]; }
uint blend_control() { return w[19] & 0xFFFFu; }
uint blend_alpha() { return w[19] >> 16; }
uint blend_brightness() { return w[20] & 0xFFFFu; }
uint master_brightness() { return w[20] >> 16; }

// --- The layers' pixels at this pixel.

struct Dot {
  uint colour;
  int priority;
  uint layer;  // 0 to 3 a background, 4 sprites, 5 the backdrop.
  bool translucent;
};

Dot top, below;
bool sprite_window;
int x;

uint shown_inside() {
  if ((control() & (7u << 13)) == 0u) return 0x3Fu;
  for (uint n = 0u; n < 2u; ++n) {
    if ((control() & (1u << (13u + n))) == 0u || (w[16] & (1u << n)) == 0u) continue;
    int x1 = int((w[15] >> (16u * n)) & 0xFFu), x2 = int((w[15] >> (16u * n + 8u)) & 0xFFu);
    int left = min(x1, x2), right = max(x1, x2);
    bool flipped = (w[16] & (4u << n)) != 0u;
    if ((x >= left && x < right) != flipped) return (window_inside() >> (8u * n)) & 0x3Fu;
  }
  if ((control() & (1u << 15)) != 0u && sprite_window) return (window_outside() >> 8) & 0x3Fu;
  return window_outside() & 0x3Fu;
}

void place(uint background, uint colour) {
  if ((shown_inside() & (1u << background)) == 0u) return;
  Dot dot = Dot(colour, int(background_control(background) & 3u), background, false);
  if (dot.priority <= top.priority) {
    below = top;
    top = dot;
  } else if (dot.priority <= below.priority) {
    below = dot;
  }
}

// The sprites at this pixel: those that are a window (window_pass), or
// those that are drawn.
void draw_sprites(bool window_pass) {
  const uint memory = second_engine ? 0x06600000u : 0x06400000u;
  const ivec2 kSizes[12] = ivec2[12](ivec2(8, 8), ivec2(16, 16), ivec2(32, 32), ivec2(64, 64),
                                     ivec2(16, 8), ivec2(32, 8), ivec2(32, 16), ivec2(64, 32),
                                     ivec2(8, 16), ivec2(8, 32), ivec2(16, 32), ivec2(32, 64));
  const uint c = control();
  for (uint i = 0u; i < 128u; ++i) {
    const uint a0 = sprite_word(i * 8u);
    const uint kind = (a0 >> 10) & 3u;
    if (((a0 >> 8) & 3u) == 2u || (kind == 2u) != window_pass) continue;
    const uint shape = a0 >> 14;
    if (shape == 3u) continue;
    const uint a1 = sprite_word(i * 8u + 2u);
    const ivec2 size = kSizes[shape * 4u + (a1 >> 14)];
    const int width = size.x, height = size.y;
    const bool matrix = (a0 & 0x100u) != 0u;
    const bool doubled = (a0 & 0x300u) == 0x300u;
    const int room_x = doubled ? width * 2 : width, room_y = doubled ? height * 2 : height;
    int y = int(a0 & 0xFFu);
    if (y >= kHeight) y -= 256;
    const int mosaic_down = int((mosaic() >> 12) & 0xFu) + 1;
    const int in_y = ((a0 & (1u << 12)) != 0u ? line - line % mosaic_down : line) - y;
    if (in_y < 0 || in_y >= room_y) continue;
    int sx = int(a1 & 0x1FFu);
    if (sx >= kWidth) sx -= 512;
    // Which pixel of its room is at this pixel.
    const bool mirrored = !matrix && (a1 & (1u << 12)) != 0u;
    const int j = mirrored ? sx + width - 1 - x : x - sx;
    if (j < 0 || j >= room_x) continue;
    const uint a2 = sprite_word(i * 8u + 4u);
    int px, py;
    if (matrix) {
      const uint set = ((a1 >> 9) & 0x1Fu) * 0x20u;
      const int mpa = signed16(sprite_word(set + 0x06u)), mpb = signed16(sprite_word(set + 0x0Eu));
      const int mpc = signed16(sprite_word(set + 0x16u)), mpd = signed16(sprite_word(set + 0x1Eu));
      px = ((mpa * (j - room_x / 2) + mpb * (in_y - room_y / 2)) >> 8) + width / 2;
      py = ((mpc * (j - room_x / 2) + mpd * (in_y - room_y / 2)) >> 8) + height / 2;
      if (px < 0 || px >= width || py < 0 || py >= height) continue;
    } else {
      px = j;
      py = (a1 & (1u << 13)) != 0u ? height - 1 - in_y : in_y;
    }

    const bool colours_256 = (a0 & (1u << 13)) != 0u;
    const bool extended = colours_256 && (c & (1u << 31)) != 0u;
    if (kind != 3u && extended && extended_palette(4u, 0u) < 0) continue;
    const bool one_dimensional = (c & (1u << 4)) != 0u;
    const uint tiles = memory + (a2 & 0x3FFu) * (one_dimensional ? 32u << ((c >> 20) & 3u) : 32u);
    const int sheet = one_dimensional ? width : (colours_256 ? 128 : 256);

    uint colour;
    if (kind == 3u) {
      uint bitmap;
      int bitmap_width;
      if ((c & (1u << 6)) != 0u) {
        bitmap = memory + (a2 & 0x3FFu) * ((c & (1u << 22)) != 0u ? 256u : 128u);
        bitmap_width = width;
      } else {
        const uint across = (c & (1u << 5)) != 0u ? 0x1Fu : 0x0Fu;
        bitmap = memory + (a2 & across) * 0x10u + (a2 & 0x3FFu & ~across) * 0x80u;
        bitmap_width = (c & (1u << 5)) != 0u ? 256 : 128;
      }
      const uint pixel = read16(bitmap + uint(py * bitmap_width + px) * 2u);
      if ((pixel & 0x8000u) == 0u) continue;
      colour = six_bits_each(pixel);
    } else if (colours_256) {
      const uint n = read8(tiles + uint(((py / 8) * sheet + py % 8) * 8 + (px / 8) * 64 + px % 8));
      if (n == 0u) continue;
      if (extended)
        colour = six_bits_each(extended_colour(4u, ((a2 & 0xF000u) >> 3) + n * 2u));
      else
        colour = colour_of(256u + n);
    } else {
      const uint byte_ = read8(tiles + uint(((py / 8) * sheet + py % 8) * 4 + (px / 8) * 32 + (px % 8) / 2));
      const uint n = (px & 1) != 0 ? byte_ >> 4 : byte_ & 0xFu;
      if (n == 0u) continue;
      colour = colour_of(256u + ((a2 >> 12) & 0xFu) * 16u + n);
    }

    if (kind == 2u) {
      sprite_window = true;
      continue;
    }
    if ((shown_inside() & 0x10u) == 0u) continue;
    const int priority = int((a2 >> 10) & 3u) - 1;
    if (top.layer == 5u || priority < top.priority) {
      top.colour = colour;
      top.priority = priority;
      top.layer = 4u;
      top.translucent = kind == 1u;
    }
  }
}

// nds::DisplayEngine::mark_3d(): where the 3D layer would be on top, or
// just below the top layer and blended with it (nds::DisplayEngine::
// kMark3DBelow, the 3D's weight at bit 18).
uint mark_3d() {
  const uint bc = blend_control();
  const uint effect = (bc >> 6) & 3u;
  const int priority = int(background_control(0u) & 3u);
  const uint inside = shown_inside();
  if ((inside & 1u) == 0u || priority > below.priority) return 0u;
  if (priority <= top.priority) {
    uint mark = kMark3D;
    if ((bc & (1u << (8u + top.layer))) != 0u)
      mark |= 1u << 24;
    else if (effect >= 2u && (bc & 1u) != 0u && (inside & 0x20u) != 0u)
      mark |= effect << 24 | blend_brightness() << 26;
    return mark;
  }
  if ((bc & (1u << 8)) != 0u &&
      (top.translucent || (effect == 1u && (bc & (1u << top.layer)) != 0u && (inside & 0x20u) != 0u)))
    return 1u << 24 | min(blend_alpha() & 0x1Fu, 16u) << 26 | min((blend_alpha() >> 8) & 0x1Fu, 16u) << 18;
  return 0u;
}

bool is_3d_below(uint mark) { return ((mark >> 24) & 0x83u) == 1u; }

void draw_tiles(uint background) {
  const uint bg_control = background_control(background);
  const uint vram = second_engine ? 0x06200000u : 0x06000000u;
  uint map = vram + ((control() >> 11) & 0x70000u) + ((bg_control << 3) & 0x0F800u);
  const uint pixels = vram + ((control() >> 8) & 0x70000u) + ((bg_control << 12) & 0x3C000u);
  const int mosaic_down = int((mosaic() >> 4) & 0xFu) + 1;
  const int from_line = (bg_control & (1u << 6)) != 0u ? line - line % mosaic_down : line;
  const uint y = (uint(from_line) + scroll_y(background)) & 0x1FFu;
  map += (y & 0xF8u) << 3;
  if (y >= 256u && (bg_control & (1u << 15)) != 0u) map += (bg_control & (1u << 14)) != 0u ? 0x1000u : 0x800u;
  const bool colours_256 = (bg_control & (1u << 7)) != 0u;
  const uint slot = background + ((background < 2u && (bg_control & (1u << 13)) != 0u) ? 2u : 0u);
  const bool extended = colours_256 && (control() & (1u << 30)) != 0u && extended_palette(slot, 0u) >= 0;

  const uint x_in = (uint(x) + scroll_x(background)) & 0x1FFu;
  uint entry_at = map + ((x_in & 0xF8u) >> 2);
  if (x_in >= 256u && (bg_control & (1u << 14)) != 0u) entry_at += 0x800u;
  const uint entry = read16(entry_at);
  const uint row = (entry & (1u << 11)) != 0u ? 7u - (y & 7u) : (y & 7u);
  const uint column = (entry & (1u << 10)) != 0u ? 7u - (x_in & 7u) : (x_in & 7u);
  uint number;
  if (colours_256) {
    number = read8(pixels + (entry & 0x3FFu) * 64u + row * 8u + column);
  } else {
    const uint b = read8(pixels + (entry & 0x3FFu) * 32u + row * 4u + column / 2u);
    number = (column & 1u) != 0u ? b >> 4 : b & 0xFu;
  }
  if (number == 0u) return;
  uint colour;
  if (extended)
    colour = six_bits_each(extended_colour(slot, ((entry >> 3) & 0x1E00u) + number * 2u));
  else if (colours_256)
    colour = colour_of(number);
  else
    colour = colour_of(((entry >> 12) & 0xFu) * 16u + number);
  place(background, colour);
}

// Where this pixel is on matrix background `background`.
ivec2 through_matrix(uint background) {
  const uint n = background - 2u;
  return ivec2((line_x(n) + pa(n) * x) >> 8, (line_y(n) + pc(n) * x) >> 8);
}

void draw_matrix_tiles(uint background) {
  const uint bg_control = background_control(background);
  const uint vram = second_engine ? 0x06200000u : 0x06000000u;
  const uint map = vram + ((bg_control << 3) & 0x0F800u) + ((control() >> 11) & 0x70000u);
  const uint pixels = vram + ((bg_control << 12) & 0x3C000u) + ((control() >> 8) & 0x70000u);
  const int size = 128 << ((bg_control >> 14) & 3u);
  ivec2 p = through_matrix(background);
  if ((bg_control & (1u << 13)) != 0u) {
    p &= ivec2(size - 1);
  } else if (p.x < 0 || p.x >= size || p.y < 0 || p.y >= size) {
    return;
  }
  const uint tile = read8(map + uint((p.y / 8) * (size / 8) + p.x / 8));
  const uint number = read8(pixels + tile * 64u + uint((p.y & 7) * 8 + (p.x & 7)));
  if (number != 0u) place(background, colour_of(number));
}

void draw_extended(uint background) {
  const uint bg_control = background_control(background);
  const uint vram = second_engine ? 0x06200000u : 0x06000000u;
  const bool repeated = (bg_control & (1u << 13)) != 0u;
  ivec2 p = through_matrix(background);
  if ((bg_control & (1u << 7)) != 0u) {
    const ivec2 kBitmapSizes[4] = ivec2[4](ivec2(128, 128), ivec2(256, 256), ivec2(512, 256), ivec2(512, 512));
    const uint data = vram + ((bg_control << 6) & 0x7C000u);
    const ivec2 size = kBitmapSizes[(bg_control >> 14) & 3u];
    if (repeated) p &= size - 1;
    else if (p.x < 0 || p.x >= size.x || p.y < 0 || p.y >= size.y) return;
    if ((bg_control & (1u << 2)) != 0u) {
      const uint pixel = read16(data + uint(p.y * size.x + p.x) * 2u);
      if ((pixel & 0x8000u) != 0u) place(background, six_bits_each(pixel));
    } else {
      const uint number = read8(data + uint(p.y * size.x + p.x));
      if (number != 0u) place(background, colour_of(number));
    }
  } else {
    const uint map = vram + ((bg_control << 3) & 0x0F800u) + ((control() >> 11) & 0x70000u);
    const uint pixels = vram + ((bg_control << 12) & 0x3C000u) + ((control() >> 8) & 0x70000u);
    const int size = 128 << ((bg_control >> 14) & 3u);
    const bool extended = (control() & (1u << 30)) != 0u;
    if (repeated) p &= ivec2(size - 1);
    else if (p.x < 0 || p.x >= size || p.y < 0 || p.y >= size) return;
    const uint entry = read16(map + uint((p.y / 8) * (size / 8) + p.x / 8) * 2u);
    if (extended && extended_palette(background, 0u) < 0) return;
    const int row = ((entry & (1u << 11)) != 0u ? 7 - p.y : p.y) & 7;
    const int column = ((entry & (1u << 10)) != 0u ? 7 - p.x : p.x) & 7;
    const uint number = read8(pixels + (entry & 0x3FFu) * 64u + uint(row * 8 + column));
    if (number == 0u) return;
    if (extended)
      place(background, six_bits_each(extended_colour(background, ((entry >> 3) & 0x1E00u) + number * 2u)));
    else
      place(background, colour_of(number));
  }
}

void draw_large(uint background) {
  const uint bg_control = background_control(background);
  const uint vram = second_engine ? 0x06200000u : 0x06000000u;
  const ivec2 size = ((bg_control >> 14) & 3u) != 0u ? ivec2(1024, 512) : ivec2(512, 1024);
  ivec2 p = through_matrix(background);
  if ((bg_control & (1u << 13)) != 0u) p &= size - 1;
  else if (p.x < 0 || p.x >= size.x || p.y < 0 || p.y >= size.y) return;
  if (second_engine) p.y &= size.y / 4 - 1;
  const uint number = read8(vram + uint(p.y * size.x + p.x));
  if (number != 0u) place(background, colour_of(number));
}

uint mix_parts(uint a, uint weight_a, uint b, uint weight_b, uint over) {
  uint result = 0u;
  for (uint shift = 0u; shift < 18u; shift += 6u)
    result |= min((part_of(a, shift) * weight_a + part_of(b, shift) * weight_b) / over, 63u) << shift;
  return result;
}

void blend() {
  const uint bc = blend_control();
  const uint effect = (bc >> 6) & 3u;
  const bool first = (bc & (1u << top.layer)) != 0u;
  const bool second = (bc & (1u << (8u + below.layer))) != 0u;
  bool alpha = false;
  if (top.translucent) {
    if (second) alpha = true;
    else if (effect < 2u || !first) return;
  } else if (effect == 0u || !first || (effect == 1u && !second)) {
    return;
  }
  if (!alpha && (shown_inside() & 0x20u) == 0u) return;
  if (alpha || effect == 1u) {
    const uint weight_top = min(blend_alpha() & 0x1Fu, 16u), weight_below = min((blend_alpha() >> 8) & 0x1Fu, 16u);
    top.colour = mix_parts(top.colour, weight_top, below.colour, weight_below, 16u);
  } else if (blend_brightness() != 0u) {
    uint result = 0u;
    for (uint shift = 0u; shift < 18u; shift += 6u) {
      const uint part = part_of(top.colour, shift);
      result |= (effect == 2u ? part + (63u - part) * blend_brightness() / 16u : part - part * blend_brightness() / 16u)
                << shift;
    }
    top.colour = result;
  }
}

uint eight_bits(uint six) { return (six & 0x3Fu) << 2 | (six & 0x3Fu) >> 4; }

void main() {
  x = min(int(v_place.x * float(kWidth)), kWidth - 1);
  line = min(int(v_place.y * float(kHeight)), kHeight - 1);
  for (int i = 0; i < 24; ++i) w[i] = lines[uint(line) * 24u + uint(i)];
  palette_at = w[21] * 256u;
  sprites_at = w[22] * 256u;
  second_engine = u_engine.x != 0u;

  top = Dot(colour_of(0u), 4, 5u, false);
  below = top;
  sprite_window = false;
  const uint c = control();
  if ((c & (1u << 12)) != 0u) {
    if ((c & (1u << 15)) != 0u) draw_sprites(true);
    draw_sprites(false);
  }
  uint mark = 0u;
  const uint mode = c & 7u;
  const bool shown0 = (c & (1u << 8)) != 0u, shown1 = (c & (1u << 9)) != 0u;
  const bool shown2 = (c & (1u << 10)) != 0u, shown3 = (c & (1u << 11)) != 0u;
  if (mode == 6u) {
    if (shown2) draw_large(2u);
  } else if (mode != 7u) {
    if (shown3) {
      if (mode == 0u) draw_tiles(3u);
      else if (mode <= 2u) draw_matrix_tiles(3u);
      else draw_extended(3u);
    }
    if (shown2) {
      if (mode == 0u || mode == 1u || mode == 3u) draw_tiles(2u);
      else if (mode == 2u || mode == 4u) draw_matrix_tiles(2u);
      else draw_extended(2u);
    }
    if (shown1) draw_tiles(1u);
    if (shown0) {
      // The main engine's 3D layer: marked, for the graphics hardware's
      // scene (the processor's picture is not drawn here).
      if (!second_engine && (c & (1u << 3)) != 0u) mark = mark_3d();
      else draw_tiles(0u);
    }
  }
  if (!is_3d_below(mark)) blend();

  uint shown;
  const uint display_mode = (c >> 16) & 3u;
  if (display_mode == 0u) {
    shown = kWhite;
  } else if (display_mode == 1u) {
    shown = top.colour;
  } else if (display_mode == 2u) {
    shown = six_bits_each(read16(0x06800000u + ((c >> 1) & 0x60000u) + uint(line * kWidth + x) * 2u));
  } else {
    shown = 0u;
  }
  const uint direction = master_brightness() >> 14;
  const uint factor = min(master_brightness() & 0x1Fu, 16u);
  if ((direction == 1u || direction == 2u) && factor != 0u && !(display_mode == 1u && is_3d_below(mark))) {
    uint result = 0u;
    for (uint shift = 0u; shift < 18u; shift += 6u) {
      const uint part = part_of(shown, shift);
      result |= (direction == 1u ? part + (63u - part) * factor / 16u : part - part * factor / 16u) << shift;
    }
    shown = result;
  }
  if (display_mode != 1u || u_engine.y != 0u) mark = 0u;
  uvec4 bytes = uvec4(eight_bits(shown), eight_bits(shown >> 6), eight_bits(shown >> 12), mark >> 24);
  if (is_3d_below(mark)) {
    // nds::DisplayEngine::for_hardware(): the 3D's weight in the low bits.
    const uint weight = (mark >> 18) & 31u;
    bytes.rgb = (bytes.rgb & uvec3(0xFDu, 0xFCu, 0xFCu)) | uvec3((weight >> 4) << 1, (weight >> 2) & 3u, weight & 3u);
  }
  o_colour = vec4(bytes) / 255.0;
}
