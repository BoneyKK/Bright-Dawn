// A 2D display engine: backgrounds, their mixing, windows, effects and what
// is shown. The expectations are worked out by hand.
#include <array>
#include <cstdint>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/display_engine.h"
#include "nds/video_memory.h"
#include "test.h"

namespace {

using bd::nds::DisplayEngine;
using bd::nds::VideoMemory;
using u32 = std::uint32_t;

// Colours as written (five bits a part) and as shown (six: doubled).
constexpr u32 kRed15 = 0x001F, kGreen15 = 0x03E0, kBlue15 = 0x7C00, kGrey15 = 0x4210;
constexpr u32 rgb(u32 r, u32 g, u32 b) { return r | g << 6 | b << 12; }
constexpr u32 kRed = rgb(62, 0, 0), kGreen = rgb(0, 62, 0), kBlue = rgb(0, 0, 62), kGrey = rgb(32, 32, 32);
constexpr u32 kWhite = 0x3FFFF;

// The control register: a mode, backgrounds shown, the picture displayed.
constexpr u32 show(unsigned background) { return 1u << (8 + background); }
constexpr u32 kPicture = 1u << 16;

struct Fixture {
  bd::nds::Arm9Io io;
  VideoMemory video;
  std::array<std::uint8_t, 0x400> palette{};
  std::array<std::uint8_t, 0x400> sprites{};
  DisplayEngine engine;
  const u32 base, vram;

  explicit Fixture(DisplayEngine::Which which = DisplayEngine::kMain)
      : engine(which, video, palette.data(), sprites.data()),
        base(DisplayEngine::kBases[which]),
        vram(which == DisplayEngine::kMain ? 0x06000000 : 0x06200000) {
    engine.attach(io);
    // Banks A to D are the main engine's backgrounds, 512 KiB, and bank E its
    // sprites; or banks C and D are the second engine's.
    if (which == DisplayEngine::kMain) {
      for (unsigned bank = 0; bank < 4; ++bank)
        video.set_control(static_cast<VideoMemory::Bank>(bank), static_cast<std::uint8_t>(VideoMemory::kEnable | 1 | bank << 3));
      video.set_control(VideoMemory::kE, VideoMemory::kEnable | 2);
    } else {
      video.set_control(VideoMemory::kC, VideoMemory::kEnable | 4);
      video.set_control(VideoMemory::kD, VideoMemory::kEnable | 4);
    }
    colour(0, kGrey15);  // The backdrop.
  }
  void reg(u32 offset, unsigned size, u32 value) { io.write(base + offset, size, value); }
  void control(u32 value) { reg(DisplayEngine::kControl, 4, value); }
  void background(unsigned n, u32 value) { reg(DisplayEngine::kBackgroundControl + 2 * n, 2, value); }
  void colour(unsigned index, u32 value) {
    palette[index * 2] = static_cast<std::uint8_t>(value);
    palette[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
  }
  void write(u32 offset, unsigned size, u32 value) { video.write(vram + offset, size, value); }
  // A map entry of a tiled background whose map is in block `block` (of
  // 2 KiB), and a row of a 16-colour tile in block `block` (of 16 KiB).
  void map(unsigned block, unsigned column, unsigned row, u32 entry) { write(block * 0x800 + (row * 32 + column) * 2, 2, entry); }
  void tile_row(unsigned block, unsigned tile, unsigned row, u32 nibbles) { write(block * 0x4000 + tile * 32 + row * 4, 4, nibbles); }
  void tile_dot(unsigned block, unsigned tile, unsigned row, unsigned column, u32 number) {
    write(block * 0x4000 + tile * 64 + row * 8 + column, 1, number);
  }
  // Draws lines 0 to `last` of a frame, as the console would.
  void draw(int last = 0) {
    engine.start_frame();
    for (int line = 0; line <= last; ++line) {
      engine.start_line(line);
      engine.draw_line(line);
    }
  }
  u32 at(int x, int line = 0) const { return engine.screen()[static_cast<std::size_t>(line * 256 + x)]; }

  // Sprites: the three attribute words of sprite `n`, a byte of the
  // sprites' pixel memory, a colour of the sprites' palette, and one of the
  // 32 matrices.
  void sprite(unsigned n, u32 a0, u32 a1, u32 a2) {
    const u32 words[3] = {a0, a1, a2};
    for (unsigned i = 0; i < 3; ++i) {
      sprites[n * 8 + i * 2] = static_cast<std::uint8_t>(words[i]);
      sprites[n * 8 + i * 2 + 1] = static_cast<std::uint8_t>(words[i] >> 8);
    }
  }
  void sprite_byte(u32 offset, u32 value) { video.write(vram + 0x400000 + offset, 1, value); }
  void sprite_word(u32 offset, u32 value) { video.write(vram + 0x400000 + offset, 2, value); }
  void sprite_colour(unsigned index, u32 value) { colour(256 + index, value); }
  void matrix(unsigned set, int pa, int pb, int pc, int pd) {
    const int numbers[4] = {pa, pb, pc, pd};
    for (unsigned i = 0; i < 4; ++i) {
      sprites[set * 0x20 + 6 + i * 8] = static_cast<std::uint8_t>(numbers[i]);
      sprites[set * 0x20 + 7 + i * 8] = static_cast<std::uint8_t>(numbers[i] >> 8);
    }
  }
  // Sprite palette 1 has red, green, blue and white as colours 1 to 4, and
  // tile 3 (16 colours) has a top row of red, green, blue.
  void simple_sprite_tile() {
    sprite_colour(17, kRed15);
    sprite_colour(18, kGreen15);
    sprite_colour(19, kBlue15);
    sprite_colour(20, 0x7FFF);
    sprite_byte(3 * 32, 0x21);
    sprite_byte(3 * 32 + 1, 0x03);
  }

  // Background 0 as 16-colour tiles, map in block 1 and tiles in block 1,
  // with one tile (number 1, palette 2) at the top left whose top row is
  // red, green, blue and then nothing.
  void simple_tiles() {
    background(0, 1u << 8 | 1u << 2);
    map(1, 0, 0, 0x2001);
    tile_row(1, 1, 0, 0x00000321);
    colour(33, kRed15);
    colour(34, kGreen15);
    colour(35, kBlue15);
  }
};

}  // namespace

TEST(display_registers) {
  Fixture f;
  // The control register, the backgrounds' and those of windows and effects
  // can be read back, with the bits that exist.
  f.control(0xFFFFFFFF);
  CHECK(f.io.read(0x04000000, 4) == 0xFFFFFFFF);
  f.background(3, 0xFFFF);
  CHECK(f.io.read(0x0400000E, 2) == 0xFFFF && f.io.read(0x04000008, 2) == 0);
  f.reg(DisplayEngine::kWindowInside, 2, 0xFFFF);
  f.reg(DisplayEngine::kWindowOutside, 2, 0xFFFF);
  f.reg(DisplayEngine::kBlendControl, 2, 0xFFFF);
  f.reg(DisplayEngine::kBlendAlpha, 2, 0xFFFF);
  f.reg(DisplayEngine::kMasterBrightness, 2, 0xFFFF);
  CHECK(f.io.read(0x04000048, 2) == 0x3F3F && f.io.read(0x0400004A, 2) == 0x3F3F);
  CHECK(f.io.read(0x04000050, 2) == 0x3FFF && f.io.read(0x04000052, 2) == 0x1F1F && f.io.read(0x0400006C, 2) == 0xC01F);
  // The rest are written only. A program writes pairs of them as one word.
  f.reg(DisplayEngine::kScroll, 4, 0x01230045);
  f.reg(DisplayEngine::kMatrix, 4, 0x01000100);
  f.reg(DisplayEngine::kMatrix + 8, 4, 0x12345678);
  f.reg(DisplayEngine::kWindowAcross, 4, 0x10203040);
  f.reg(DisplayEngine::kMosaic, 2, 0x1111);
  f.reg(DisplayEngine::kBlendBrightness, 2, 0x10);
  CHECK(f.io.read(0x04000010, 4) == 0 && f.io.read(0x04000020, 4) == 0 && f.io.read(0x04000040, 4) == 0);
  CHECK(f.io.unmodelled().empty());

  // The second engine's are 0x1000 on. It has no 3D, no display from a bank
  // and no choice of where its tiles are.
  Fixture second(DisplayEngine::kSub);
  second.control(0xFFFFFFFF);
  CHECK(second.io.read(0x04001000, 4) == 0xC0B1FFF7 && second.io.read(0x04000000, 4) == 0);
}

TEST(display_off_is_white_and_an_empty_picture_is_the_backdrop) {
  Fixture f;
  f.draw();
  CHECK(f.at(0) == kWhite && f.at(255) == kWhite);
  // Colour 0 of the palette; five bits a part are doubled.
  f.control(kPicture);
  f.draw(1);
  CHECK(f.at(0) == kGrey && f.at(255, 1) == kGrey);
  f.colour(0, 0x7FFF);
  f.draw();
  CHECK(f.at(0) == rgb(62, 62, 62));
}

TEST(display_tiles_of_16_colours) {
  Fixture f;
  f.simple_tiles();
  f.control(0 | show(0) | kPicture);
  f.draw(1);
  // The tile's palette is the map entry's top four bits; colour 0 of a tile
  // is see-through.
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kBlue && f.at(3) == kGrey && f.at(8) == kGrey);
  CHECK(f.at(0, 1) == kGrey);
  // The same tile in the next place across, and in the second row of tiles.
  f.map(1, 1, 0, 0x2001);
  f.map(1, 0, 1, 0x2001);
  f.draw(8);
  CHECK(f.at(8) == kRed && f.at(10) == kBlue && f.at(0, 8) == kRed && f.at(2, 8) == kBlue);
  // Mirrored across (bit 10), and upside down (bit 11).
  f.map(1, 1, 0, 0x2401);
  f.map(1, 0, 1, 0x2801);
  f.draw(15);
  CHECK(f.at(15) == kRed && f.at(14) == kGreen && f.at(13) == kBlue && f.at(8) == kGrey);
  CHECK(f.at(0, 8) == kGrey && f.at(0, 15) == kRed && f.at(2, 15) == kBlue);
}

TEST(display_tiles_scroll_and_wrap) {
  Fixture f;
  f.simple_tiles();
  f.control(0 | show(0) | kPicture);
  // One pixel to the left: the tile's first pixel is off the screen.
  f.reg(DisplayEngine::kScroll, 2, 1);
  f.draw();
  CHECK(f.at(0) == kGreen && f.at(1) == kBlue && f.at(2) == kGrey);
  // The background is 256 across and repeats: scrolled 250, its start is at
  // pixel 6.
  f.reg(DisplayEngine::kScroll, 2, 250);
  f.draw();
  CHECK(f.at(6) == kRed && f.at(8) == kBlue && f.at(0) == kGrey);
  // Scrolled eight lines up (the scroll is nine bits): the top row is on line 8.
  f.reg(DisplayEngine::kScroll, 2, 0);
  f.reg(DisplayEngine::kScroll + 2, 2, 504);
  f.draw(8);
  CHECK(f.at(0) == kGrey && f.at(0, 8) == kRed);

  // A background 512 across has a second map after the first, for its right
  // half; one 512 down, for its lower half.
  f.reg(DisplayEngine::kScroll + 2, 2, 0);
  f.map(2, 0, 0, 0x2401);
  f.background(0, 1u << 8 | 1u << 2 | 1u << 14);
  f.reg(DisplayEngine::kScroll, 2, 256);
  f.draw();
  CHECK(f.at(7) == kRed && f.at(5) == kBlue && f.at(0) == kGrey);
  f.reg(DisplayEngine::kScroll, 2, 0);
  f.draw();
  CHECK(f.at(0) == kRed);
  f.background(0, 1u << 8 | 1u << 2 | 1u << 15);
  f.reg(DisplayEngine::kScroll + 2, 2, 256);
  f.draw();
  CHECK(f.at(7) == kRed && f.at(0) == kGrey);
}

TEST(display_tiles_of_256_colours_and_extended_palettes) {
  Fixture f;
  // A tile is 64 bytes, a byte a pixel.
  f.background(0, 1u << 8 | 1u << 2 | 1u << 7);
  f.map(1, 0, 0, 0x3001);
  f.tile_dot(1, 1, 0, 0, 200);
  f.tile_dot(1, 1, 0, 1, 201);
  f.tile_dot(1, 1, 0, 7, 200);
  f.colour(200, kRed15);
  f.colour(201, kGreen15);
  f.control(0 | show(0) | kPicture);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kGrey && f.at(7) == kRed);
  // With extended palettes the map entry's top four bits choose one of
  // sixteen palettes of 256 colours, in the background's slot.
  f.video.set_control(VideoMemory::kF, VideoMemory::kEnable);
  f.video.write(0x06890000 + 3 * 0x200 + 200 * 2, 2, kBlue15);
  f.video.write(0x06890000 + 3 * 0x200 + 201 * 2, 2, 0x7FFF);
  f.video.set_control(VideoMemory::kF, VideoMemory::kEnable | 4);
  f.control(0 | show(0) | kPicture | DisplayEngine::kExtendedPalettes);
  f.draw();
  CHECK(f.at(0) == kBlue && f.at(1) == rgb(62, 62, 62));
  // Background 0 can use the slot two on; nothing is there, so the standard
  // palette it is.
  f.background(0, 1u << 8 | 1u << 2 | 1u << 7 | 1u << 13);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen);
}

TEST(display_the_two_topmost_layers_by_priority) {
  Fixture f;
  f.simple_tiles();
  // Background 1: a tile whose top row is blue for four pixels, from pixel 1.
  f.background(1, 2u << 8 | 2u << 2);
  f.map(2, 0, 0, 0x2001);
  f.tile_row(2, 1, 0, 0x00033330);
  f.control(0 | show(0) | show(1) | kPicture);
  // Equal priorities: the lower number is on top.
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kBlue && f.at(3) == kBlue && f.at(4) == kBlue && f.at(5) == kGrey);
  // Background 0 put behind (priority 1 against 0).
  f.background(0, 1u << 8 | 1u << 2 | 1);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kBlue && f.at(2) == kBlue && f.at(4) == kBlue);
  // A background that is not shown is not there.
  f.control(0 | show(0) | kPicture);
  f.draw();
  CHECK(f.at(1) == kGreen && f.at(4) == kGrey);
}

TEST(display_the_3d_picture_is_background_0) {
  Fixture f;
  std::vector<u32> picture(256 * 192, 0);
  picture[0] = rgb(63, 0, 0) | 63u << 18;  // Solid.
  picture[1] = rgb(63, 0, 0) | 31u << 18;  // Half there.
  picture[2] = rgb(63, 0, 0);              // Not there.
  f.engine.set_3d_picture(&picture);
  f.colour(0, kBlue15);
  f.control(0 | DisplayEngine::kBackground0Is3D | show(0) | kPicture);
  f.draw();
  // Without effects a 3D pixel shows as it is, whatever its alpha.
  CHECK(f.at(0) == rgb(63, 0, 0) && f.at(1) == rgb(63, 0, 0) && f.at(2) == kBlue);
  // Over a layer that can be blended with (here the backdrop), it is mixed
  // by its own alpha: 32 of 64 parts of each.
  f.reg(DisplayEngine::kBlendControl, 2, 1u << 13);
  f.draw();
  CHECK(f.at(0) == rgb(63, 0, 0) && f.at(1) == rgb(31, 0, 31) && f.at(2) == kBlue);
  // Brightening applies to it like to any top layer: half way to white.
  f.reg(DisplayEngine::kBlendControl, 2, 1u | 2u << 6);
  f.reg(DisplayEngine::kBlendBrightness, 2, 8);
  f.draw();
  CHECK(f.at(0) == rgb(63, 31, 31) && f.at(2) == kBlue);
  // Without the bit, background 0 is tiles; with no picture, nothing.
  f.reg(DisplayEngine::kBlendControl, 2, 0);
  f.engine.set_3d_picture(nullptr);
  f.draw();
  CHECK(f.at(0) == kBlue);
  // The second engine has no 3D.
  Fixture second(DisplayEngine::kSub);
  second.engine.set_3d_picture(&picture);
  second.control(0 | DisplayEngine::kBackground0Is3D | show(0) | kPicture);
  second.draw();
  CHECK(second.at(0) == kGrey);
}

TEST(display_effects) {
  Fixture f;
  f.simple_tiles();
  f.background(1, 2u << 8 | 2u << 2);
  f.map(2, 0, 0, 0x2001);
  f.tile_row(2, 1, 0, 0x00000022);  // Green under the first two pixels.
  f.control(0 | show(0) | show(1) | kPicture);
  // Alpha blending of background 0 over background 1: eight sixteenths of
  // each.
  f.reg(DisplayEngine::kBlendControl, 2, 1u | 1u << 6 | 1u << 9);
  f.reg(DisplayEngine::kBlendAlpha, 2, 8 | 8u << 8);
  f.draw();
  CHECK(f.at(0) == rgb(31, 31, 0) && f.at(1) == rgb(0, 62, 0));
  // Not over the backdrop, which is not chosen as the layer below.
  CHECK(f.at(2) == kBlue);
  // Sixteen sixteenths of each: the sum, held at 63. More than 16 counts as 16.
  f.reg(DisplayEngine::kBlendAlpha, 2, 31 | 31u << 8);
  f.draw();
  CHECK(f.at(0) == rgb(62, 62, 0) && f.at(1) == rgb(0, 63, 0));
  // Brightening and darkening of the top layer, in sixteenths.
  f.reg(DisplayEngine::kBlendControl, 2, 1u | 2u << 6);
  f.reg(DisplayEngine::kBlendBrightness, 2, 8);
  f.draw();
  CHECK(f.at(0) == rgb(62, 31, 31) && f.at(3) == kGrey);
  f.reg(DisplayEngine::kBlendBrightness, 2, 31);
  f.draw();
  CHECK(f.at(0) == kWhite);
  f.reg(DisplayEngine::kBlendControl, 2, 1u | 3u << 6);
  f.reg(DisplayEngine::kBlendBrightness, 2, 8);
  f.draw();
  CHECK(f.at(0) == rgb(31, 0, 0) && f.at(1) == rgb(0, 31, 0));
  // The backdrop can be the top layer of an effect too.
  f.reg(DisplayEngine::kBlendControl, 2, 1u << 5 | 3u << 6);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(3) == rgb(16, 16, 16));
}

TEST(display_windows) {
  Fixture f;
  f.simple_tiles();
  f.tile_row(1, 1, 0, 0x11111111);
  for (unsigned column = 1; column < 32; ++column) f.map(1, column, 0, 0x2001);
  for (unsigned row = 1; row < 8; ++row) f.tile_row(1, 1, row, 0x11111111);
  // Window 0: pixels 8 to 15 of lines 2 to 4. Background 0 shows inside it
  // only.
  f.control(0 | show(0) | kPicture | DisplayEngine::kWindow0);
  f.reg(DisplayEngine::kWindowAcross, 2, 8u << 8 | 16);
  f.reg(DisplayEngine::kWindowAcross + 4, 2, 2u << 8 | 5);
  f.reg(DisplayEngine::kWindowInside, 2, 0x01);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x00);
  f.draw(6);
  CHECK(f.at(8, 1) == kGrey && f.at(8, 2) == kRed && f.at(15, 4) == kRed && f.at(8, 5) == kGrey);
  CHECK(f.at(7, 3) == kGrey && f.at(16, 3) == kGrey);
  // Outside only.
  f.reg(DisplayEngine::kWindowInside, 2, 0x00);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x01);
  f.draw(6);
  CHECK(f.at(8, 3) == kGrey && f.at(7, 3) == kRed && f.at(8, 1) == kRed);
  // With its edges the other way round, the window is what is not between them.
  f.reg(DisplayEngine::kWindowInside, 2, 0x01);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x00);
  f.reg(DisplayEngine::kWindowAcross, 2, 16u << 8 | 8);
  f.draw(6);
  CHECK(f.at(8, 3) == kGrey && f.at(7, 3) == kRed && f.at(16, 3) == kRed && f.at(7, 1) == kGrey);
  // Window 1 counts where window 0 is not.
  f.control(0 | show(0) | kPicture | DisplayEngine::kWindow0 | DisplayEngine::kWindow1);
  f.reg(DisplayEngine::kWindowAcross, 2, 8u << 8 | 16);
  f.reg(DisplayEngine::kWindowAcross + 2, 2, 12u << 8 | 24);
  f.reg(DisplayEngine::kWindowAcross + 6, 2, 0u << 8 | 4);
  f.reg(DisplayEngine::kWindowInside, 2, 0x0100);
  f.draw(6);
  CHECK(f.at(12, 3) == kGrey && f.at(16, 3) == kRed && f.at(23, 3) == kRed && f.at(24, 3) == kGrey && f.at(16, 4) == kGrey);
  // A window says whether effects apply inside it (bit 5).
  f.control(0 | show(0) | kPicture | DisplayEngine::kWindow0);
  f.reg(DisplayEngine::kBlendControl, 2, 1u | 3u << 6);
  f.reg(DisplayEngine::kBlendBrightness, 2, 8);
  f.reg(DisplayEngine::kWindowInside, 2, 0x01);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x21);
  f.draw(6);
  CHECK(f.at(8, 3) == kRed && f.at(7, 3) == rgb(31, 0, 0));
}

TEST(display_tiles_through_a_matrix) {
  Fixture f;
  // Mode 2: background 2 is tiles of 256 colours seen through a matrix, 128
  // pixels square, with a map of bytes. Tile 1 is at the top left: its
  // first row is red, green, and its second row blue.
  f.background(2, 1u << 8 | 1u << 2);
  f.write(0x800, 1, 1);
  f.tile_dot(1, 1, 0, 0, 1);
  f.tile_dot(1, 1, 0, 1, 2);
  f.tile_dot(1, 1, 1, 0, 3);
  f.colour(1, kRed15);
  f.colour(2, kGreen15);
  f.colour(3, kBlue15);
  f.control(2 | show(2) | kPicture);
  // The matrix that changes nothing.
  f.reg(DisplayEngine::kMatrix, 4, 0x00000100);
  f.reg(DisplayEngine::kMatrix + 4, 4, 0x01000000);
  f.draw(1);
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kGrey && f.at(0, 1) == kBlue && f.at(1, 1) == kGrey);
  // Twice as large: half a pixel of the background a pixel of the screen.
  f.reg(DisplayEngine::kMatrix, 2, 0x80);
  f.reg(DisplayEngine::kMatrix + 6, 2, 0x80);
  f.draw(3);
  CHECK(f.at(0) == kRed && f.at(1) == kRed && f.at(2) == kGreen && f.at(3) == kGreen && f.at(4) == kGrey);
  CHECK(f.at(0, 1) == kRed && f.at(0, 2) == kBlue && f.at(1, 3) == kBlue);
  // Moved: the reference point is where the screen's top left is on the
  // background, in 256ths of a pixel, 28 bits with a sign.
  f.reg(DisplayEngine::kMatrix, 2, 0x100);
  f.reg(DisplayEngine::kMatrix + 6, 2, 0x100);
  f.reg(DisplayEngine::kMatrix + 8, 4, 0x0FFFFE00);  // -2 pixels.
  f.draw();
  CHECK(f.at(0) == kGrey && f.at(1) == kGrey && f.at(2) == kRed && f.at(3) == kGreen);
  // Outside the background is nothing, or, if asked, the background again.
  f.reg(DisplayEngine::kMatrix + 8, 4, 126u << 8);
  f.draw();
  CHECK(f.at(0) == kGrey && f.at(2) == kGrey);
  f.background(2, 1u << 8 | 1u << 2 | 1u << 13);
  f.draw();
  CHECK(f.at(0) == kGrey && f.at(2) == kRed && f.at(3) == kGreen);
  // A turn by a quarter: across the screen is down the background.
  f.reg(DisplayEngine::kMatrix + 8, 4, 0);
  f.reg(DisplayEngine::kMatrix, 4, 0x01000000);      // PA 0, PB 0x100.
  f.reg(DisplayEngine::kMatrix + 4, 4, 0x00000100);  // PC 0x100, PD 0.
  f.draw(1);
  CHECK(f.at(0) == kRed && f.at(1) == kBlue && f.at(0, 1) == kGreen);
}

TEST(display_bitmaps_and_tiles_with_full_map_entries) {
  Fixture f;
  f.control(5 | show(2) | kPicture);
  f.reg(DisplayEngine::kMatrix, 4, 0x00000100);
  f.reg(DisplayEngine::kMatrix + 4, 4, 0x01000000);
  // A bitmap of 16 bits a pixel, 256 square, in block 1 (of 16 KiB). A
  // pixel is there if its top bit is set.
  f.background(2, 1u << 14 | 1u << 8 | 1u << 7 | 1u << 2);
  f.write(0x4000, 2, 0x8000 | kRed15);
  f.write(0x4002, 2, kGreen15);
  f.write(0x4000 + (256 + 3) * 2, 2, 0x8000 | kBlue15);
  f.draw(1);
  CHECK(f.at(0) == kRed && f.at(1) == kGrey && f.at(3, 1) == kBlue);
  // A bitmap of a byte a pixel, with the palette. The same memory read
  // that way is colour numbers 0x1F and 0x80 (which is black), then 0xE0
  // and 3; only colour 0 is see-through.
  f.background(2, 1u << 14 | 1u << 8 | 1u << 7);
  f.colour(0x1F, kGreen15);
  f.colour(3, kBlue15);
  f.draw();
  CHECK(f.at(0) == kGreen && f.at(1) == 0 && f.at(2) == 0 && f.at(3) == kBlue && f.at(4) == kGrey);
  // 512 across and 256 down.
  f.background(2, 2u << 14 | 1u << 8 | 1u << 7);
  f.write(0x4000 + 300, 1, 0x1F);
  f.reg(DisplayEngine::kMatrix + 8, 4, 256u << 8);
  f.draw();
  CHECK(f.at(44) == kGreen && f.at(0) == kGrey);

  // Tiles through the matrix with map entries of 16 bits: mirrored tiles,
  // and with extended palettes sixteen palettes of 256 colours.
  f.reg(DisplayEngine::kMatrix + 8, 4, 0);
  f.background(2, 2u << 8 | 2u << 2);
  f.write(0x1000, 2, 0x0401);  // Tile 1, mirrored across.
  f.tile_dot(2, 1, 0, 0, 5);
  f.colour(5, kRed15);
  f.draw();
  CHECK(f.at(7) == kRed && f.at(0) == kGrey);
  // Extended palettes on with no bank for them: nothing is drawn.
  f.control(5 | show(2) | kPicture | DisplayEngine::kExtendedPalettes);
  f.draw();
  CHECK(f.at(7) == kGrey);
  // Background 2 uses slot 2: bank F with an offset.
  f.video.set_control(VideoMemory::kF, VideoMemory::kEnable);
  f.video.write(0x06890000 + 5 * 2, 2, kBlue15);
  f.video.set_control(VideoMemory::kF, VideoMemory::kEnable | 4 | 1u << 3);
  f.draw();
  CHECK(f.at(7) == kBlue);
}

TEST(display_one_large_bitmap) {
  Fixture f;
  // Mode 6: background 2 is one bitmap of a byte a pixel, 512 across and
  // 1024 down, taking all 512 KiB.
  f.control(6 | show(2) | kPicture);
  f.reg(DisplayEngine::kMatrix, 4, 0x00000100);
  f.reg(DisplayEngine::kMatrix + 4, 4, 0x01000000);
  f.colour(9, kRed15);
  f.write(5, 1, 9);
  f.write(600 * 512 + 300, 1, 9);
  f.draw();
  CHECK(f.at(5) == kRed && f.at(4) == kGrey);
  f.reg(DisplayEngine::kMatrix + 8, 4, 256u << 8);
  f.reg(DisplayEngine::kMatrix + 12, 4, 600u << 8);
  f.draw();
  CHECK(f.at(44) == kRed && f.at(5) == kGrey);
  // The other shape: 1024 across.
  f.background(2, 1u << 14);
  f.write(300 * 1024 + 900, 1, 9);
  f.reg(DisplayEngine::kMatrix + 8, 4, 800u << 8);
  f.reg(DisplayEngine::kMatrix + 12, 4, 300u << 8);
  f.draw();
  CHECK(f.at(100) == kRed);
}

TEST(display_what_is_shown_and_the_master_brightness) {
  Fixture f;
  f.simple_tiles();
  // A bank shown directly: 16 bits a pixel from bank B, which is plain
  // memory for that.
  f.video.set_control(VideoMemory::kB, VideoMemory::kEnable);
  f.video.write(0x06820000 + 512 + 4, 2, kGreen15);
  f.control(0 | show(0) | 2u << 16 | 1u << 18);
  f.draw(1);
  CHECK(f.at(0) == 0 && f.at(2, 1) == kGreen);
  // The picture is still made, for the display capture.
  CHECK(f.engine.mixed_line()[0] == kGrey);
  f.draw();
  CHECK(f.engine.mixed_line()[0] == kRed && f.engine.mixed_line()[3] == kGrey);
  // The master brightness: sixteenths of the way to white or to black, on
  // whatever is shown.
  f.control(0 | show(0) | kPicture);
  f.reg(DisplayEngine::kMasterBrightness, 2, 1u << 14 | 8);
  f.draw();
  CHECK(f.at(0) == rgb(62, 31, 31) && f.at(3) == rgb(47, 47, 47));
  CHECK(f.engine.mixed_line()[0] == kRed);
  f.reg(DisplayEngine::kMasterBrightness, 2, 2u << 14 | 8);
  f.draw();
  CHECK(f.at(0) == rgb(31, 0, 0) && f.at(3) == rgb(16, 16, 16));
  f.reg(DisplayEngine::kMasterBrightness, 2, 2u << 14 | 31);
  f.draw();
  CHECK(f.at(0) == 0 && f.at(3) == 0);
  f.reg(DisplayEngine::kMasterBrightness, 2, 8);  // No direction: no change.
  f.draw();
  CHECK(f.at(0) == kRed);
  // What cannot be shown yet is noted.
  f.control(0 | show(0) | 3u << 16);
  f.draw();
  CHECK(f.engine.notes().count("the display fed from main memory") == 1);
}

TEST(display_the_second_engine_has_its_own_memory) {
  Fixture f(DisplayEngine::kSub);
  f.simple_tiles();
  f.control(0 | show(0) | kPicture);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kBlue && f.at(3) == kGrey);
}

TEST(display_mosaic_down_the_screen) {
  Fixture f;
  f.simple_tiles();
  f.tile_row(1, 1, 4, 0x00000002);
  f.control(0 | show(0) | kPicture);
  // Steps of four lines: lines 0 to 3 show the tile's row 0, lines 4 to 7
  // its row 4.
  f.reg(DisplayEngine::kMosaic, 2, 3u << 4);
  f.background(0, 1u << 8 | 1u << 2 | 1u << 6);
  f.draw(7);
  CHECK(f.at(0, 0) == kRed && f.at(0, 3) == kRed && f.at(0, 4) == kGreen && f.at(0, 7) == kGreen);
  f.background(0, 1u << 8 | 1u << 2);
  f.draw(7);
  CHECK(f.at(0, 3) == kGrey && f.at(0, 4) == kGreen && f.at(0, 7) == kGrey);
}

namespace {

// Sprite attributes.
constexpr u32 kSprites = DisplayEngine::kShowSprites;
constexpr u32 kTile3Palette1 = 3 | 1u << 12;
constexpr u32 kOff = 1u << 9, kWithMatrix = 1u << 8, kDoubled = 3u << 8;
constexpr u32 kTranslucent = 1u << 10, kIsWindow = 2u << 10, kBitmap = 3u << 10;
constexpr u32 kColours256 = 1u << 13;
constexpr u32 kWide = 1u << 14, kTall = 2u << 14;
constexpr u32 kMirrored = 1u << 12, kUpsideDown = 1u << 13;
constexpr u32 priority(u32 p) { return p << 10; }
constexpr u32 kAllWhite = rgb(62, 62, 62);

}  // namespace

TEST(display_sprites_of_tiles) {
  Fixture f;
  f.simple_sprite_tile();
  f.control(0 | kSprites | kPicture);
  // An 8 by 8 sprite at (10, 5).
  f.sprite(0, 5, 10, kTile3Palette1);
  // The others are switched off (all zeros would be a sprite at the top left).
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.draw(12);
  CHECK(f.at(10, 5) == kRed && f.at(11, 5) == kGreen && f.at(12, 5) == kBlue && f.at(13, 5) == kGrey);
  CHECK(f.at(10, 4) == kGrey && f.at(10, 6) == kGrey);
  // Mirrored across; upside down.
  f.sprite(0, 5, 10 | kMirrored, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(17, 5) == kRed && f.at(16, 5) == kGreen && f.at(15, 5) == kBlue && f.at(10, 5) == kGrey);
  f.sprite(0, 5, 10 | kUpsideDown, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(10, 12) == kRed && f.at(12, 12) == kBlue && f.at(10, 5) == kGrey);
  // Positions wrap: 510 across is two pixels off the left edge, and 251
  // down five lines off the top.
  f.sprite_byte(3 * 32 + 5 * 4, 0x02);
  f.sprite(0, 251, 510, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(0, 0) == kGrey && f.at(0, 5) == kGrey);
  f.sprite(0, 5, 510, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(0, 5) == kBlue && f.at(1, 5) == kGrey);
  f.sprite(0, 251, 10, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(10, 0) == kGreen && f.at(10, 5) == kGrey);
  // Switched off.
  f.sprite(0, 5 | kOff, 10, kTile3Palette1);
  f.draw(12);
  CHECK(f.at(10, 5) == kGrey);
  // With sprites not shown, none is.
  f.sprite(0, 5, 10, kTile3Palette1);
  f.control(0 | kPicture);
  f.draw(12);
  CHECK(f.at(10, 5) == kGrey);
}

TEST(display_sprite_shapes_and_how_their_tiles_are_laid_out) {
  Fixture f;
  f.simple_sprite_tile();
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.control(0 | kSprites | kPicture);
  // Tiles are numbered in steps of 32 bytes and laid out as a sheet 32 tiles
  // wide: a sprite 16 wide goes on in the next tile, one 16 tall in the tile
  // 32 further.
  f.sprite_byte(4 * 32, 0x02);
  f.sprite_byte(35 * 32, 0x01);
  f.sprite(0, 0 | kWide, 0, kTile3Palette1);
  f.draw(8);
  CHECK(f.at(0, 0) == kRed && f.at(8, 0) == kGreen && f.at(0, 8) == kGrey);
  f.sprite(0, 0 | kTall, 0, kTile3Palette1);
  f.draw(8);
  CHECK(f.at(0, 0) == kRed && f.at(8, 0) == kGrey && f.at(0, 8) == kRed);
  // One-dimensional: a sprite's tiles follow one another,
  f.control(0 | kSprites | kPicture | 1u << 4);
  f.draw(8);
  CHECK(f.at(0, 0) == kRed && f.at(0, 8) == kGreen);
  // and tiles can be numbered in larger steps: of 64 bytes, tile 3 is where
  // tile 6 was.
  f.sprite_byte(6 * 32, 0x03);
  f.control(0 | kSprites | kPicture | 1u << 4 | 1u << 20);
  f.draw(8);
  CHECK(f.at(0, 0) == kBlue);
  // The largest: 64 by 64 (shape square, size 3).
  f.control(0 | kSprites | kPicture);
  f.sprite_byte(3 * 32 + 7 * 32 + 3, 0x40);  // Its pixel (63, 0): white.
  f.sprite(0, 0, 100 | 3u << 14, kTile3Palette1);
  f.draw(0);
  CHECK(f.at(100, 0) == kRed && f.at(163, 0) == kAllWhite && f.at(164, 0) == kGrey);
}

TEST(display_sprites_of_256_colours) {
  Fixture f;
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.control(0 | kSprites | kPicture);
  // A byte a pixel; the tile's number still counts in steps of 32 bytes.
  f.sprite_byte(4 * 32, 7);
  f.sprite_byte(4 * 32 + 9, 8);
  f.sprite_colour(7, kRed15);
  f.sprite_colour(8, kGreen15);
  f.sprite(0, 0 | kColours256, 0, 4 | 2u << 12);
  f.draw(1);
  CHECK(f.at(0, 0) == kRed && f.at(1, 1) == kGreen && f.at(1, 0) == kGrey);
  // With extended palettes the sprite's palette number chooses one of
  // sixteen palettes of 256 colours; with no bank for them it is not drawn.
  f.control(0 | kSprites | kPicture | 1u << 31);
  f.draw(1);
  CHECK(f.at(0, 0) == kGrey);
  f.video.set_control(VideoMemory::kG, VideoMemory::kEnable);
  f.video.write(0x06894000 + 2 * 0x200 + 7 * 2, 2, kBlue15);
  f.video.set_control(VideoMemory::kG, VideoMemory::kEnable | 5);
  f.draw(1);
  CHECK(f.at(0, 0) == kBlue);
}

TEST(display_sprites_among_the_layers) {
  Fixture f;
  f.simple_tiles();
  f.simple_sprite_tile();
  for (unsigned n = 0; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  // A white sprite tile: number 5.
  f.sprite_byte(5 * 32, 0x44);
  f.sprite_byte(5 * 32 + 1, 0x44);
  const u32 white = 5 | 1u << 12;
  f.control(0 | show(0) | kSprites | kPicture);
  // Of the same priority, a sprite is above a background.
  f.sprite(0, 0, 0, white);
  f.draw();
  CHECK(f.at(0) == kAllWhite && f.at(3) == kAllWhite && f.at(4) == kGrey);
  // One priority down it is below it, and shows where the background is
  // see-through.
  f.sprite(0, 0, 0, white | priority(1));
  f.draw();
  CHECK(f.at(0) == kRed && f.at(2) == kBlue && f.at(3) == kAllWhite);
  f.background(0, 1u << 8 | 1u << 2 | 1);
  f.draw();
  CHECK(f.at(0) == kAllWhite);
  // Among sprites of one priority the lower number is on top;
  f.background(0, 1u << 8 | 1u << 2);
  f.control(0 | kSprites | kPicture);
  f.sprite(0, 0, 0, kTile3Palette1);
  f.sprite(1, 0, 0, white);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kBlue && f.at(3) == kAllWhite);
  // a higher priority is on top whatever the number.
  f.sprite(0, 0, 0, kTile3Palette1 | priority(2));
  f.sprite(1, 0, 0, white | priority(1));
  f.draw();
  CHECK(f.at(0) == kAllWhite);
}

TEST(display_translucent_sprites_and_the_window_sprites_make) {
  Fixture f;
  f.simple_tiles();
  f.simple_sprite_tile();
  for (unsigned n = 0; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.sprite_byte(5 * 32, 0x44);
  f.sprite_byte(5 * 32 + 1, 0x44);
  const u32 white = 5 | 1u << 12;
  f.control(0 | show(0) | kSprites | kPicture);
  // A sprite that asks to be blended is, with a layer below that allows it,
  // whatever effect is chosen: eight sixteenths of each here.
  f.reg(DisplayEngine::kBlendControl, 2, 1u << 8);
  f.reg(DisplayEngine::kBlendAlpha, 2, 8 | 8u << 8);
  f.sprite(0, 0 | kTranslucent, 0, white);
  f.draw();
  CHECK(f.at(0) == rgb(62, 31, 31) && f.at(1) == rgb(31, 62, 31) && f.at(3) == kAllWhite);
  f.sprite(0, 0, 0, white);
  f.draw();
  CHECK(f.at(0) == kAllWhite);

  // A sprite that is a window is not drawn. Where it has pixels, the layers
  // chosen for the sprites' window show.
  f.reg(DisplayEngine::kBlendControl, 2, 0);
  f.sprite_byte(6 * 32, 0x11);
  f.sprite(0, 0 | kIsWindow, 0, 6 | 1u << 12);
  f.control(0 | show(0) | kSprites | kPicture | DisplayEngine::kSpriteWindow);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x0100);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(1) == kGreen && f.at(2) == kGrey);
  // Without the sprites' window switched on, such a sprite does nothing.
  f.control(0 | show(0) | kSprites | kPicture);
  f.draw();
  CHECK(f.at(0) == kRed && f.at(2) == kBlue);
  // A window can hide sprites (bit 4).
  f.sprite(0, 0, 0, white);
  f.control(0 | kSprites | kPicture | DisplayEngine::kWindow0);
  f.reg(DisplayEngine::kWindowAcross, 2, 0u << 8 | 2);
  f.reg(DisplayEngine::kWindowAcross + 4, 2, 0u << 8 | 100);
  f.reg(DisplayEngine::kWindowInside, 2, 0x00);
  f.reg(DisplayEngine::kWindowOutside, 2, 0x10);
  f.draw();
  CHECK(f.at(0) == kGrey && f.at(1) == kGrey && f.at(2) == kAllWhite);
}

TEST(display_bitmap_sprites) {
  Fixture f;
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.control(0 | kSprites | kPicture);
  // 16 bits a pixel. Laid out as a sheet 128 pixels wide; the sprite's
  // number is its place on the sheet in steps of eight pixels: across in
  // the low four bits, down in the rest.
  f.sprite_word(0x810, 0x8000 | kRed15);
  f.sprite_word(0x812, kGreen15);  // No pixel: the top bit is clear.
  f.sprite_word(0x810 + 256, 0x8000 | kBlue15);
  f.sprite(0, 0 | kBitmap, 0, 0x11);
  f.draw(1);
  CHECK(f.at(0, 0) == kRed && f.at(1, 0) == kGrey && f.at(0, 1) == kBlue);
  // One-dimensional: the number counts in steps of 128 bytes and the
  // sprite's lines follow one another.
  f.control(0 | kSprites | kPicture | 1u << 6);
  f.sprite_word(2 * 128, 0x8000 | kGreen15);
  f.sprite_word(2 * 128 + 16, 0x8000 | kRed15);
  f.sprite(0, 0 | kBitmap, 0, 2);
  f.draw(1);
  CHECK(f.at(0, 0) == kGreen && f.at(0, 1) == kRed);
  f.sprite(0, 0 | kBitmap, 0 | kUpsideDown, 2);
  f.draw(7);
  CHECK(f.at(0, 7) == kGreen && f.at(0, 6) == kRed);
}

TEST(display_sprites_through_a_matrix) {
  Fixture f;
  f.simple_sprite_tile();
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.control(0 | kSprites | kPicture);
  // Matrix 1, the one that changes nothing.
  f.matrix(1, 0x100, 0, 0, 0x100);
  f.sprite(0, 0 | kWithMatrix, 0 | 1u << 9, kTile3Palette1);
  f.draw(1);
  CHECK(f.at(0, 0) == kRed && f.at(1, 0) == kGreen && f.at(2, 0) == kBlue && f.at(3, 0) == kGrey && f.at(0, 1) == kGrey);
  // Twice as large about its middle: without more room, only the middle
  // shows: pixels 2 to 5 of the sprite's rows 2 to 5, each two wide and two
  // high. Row 2 gets a blue pixel at 2 for this.
  f.sprite_byte(3 * 32 + 2 * 4 + 1, 0x03);
  f.matrix(1, 0x80, 0, 0, 0x80);
  f.draw(3);
  CHECK(f.at(0, 0) == kBlue && f.at(1, 0) == kBlue && f.at(2, 0) == kGrey && f.at(0, 1) == kBlue && f.at(0, 2) == kGrey);
  // With twice the room, all of it: 16 by 16 from the same corner.
  f.sprite(0, 0 | kDoubled, 0 | 1u << 9, kTile3Palette1);
  f.draw(2);
  CHECK(f.at(0, 0) == kRed && f.at(1, 0) == kRed && f.at(2, 0) == kGreen && f.at(4, 0) == kBlue && f.at(6, 0) == kGrey);
  CHECK(f.at(0, 1) == kRed && f.at(0, 2) == kGrey);
  // A quarter turn.
  f.matrix(1, 0, 0x100, 0x100, 0);
  f.sprite(0, 0 | kWithMatrix, 0 | 1u << 9, kTile3Palette1);
  f.draw(2);
  CHECK(f.at(0, 0) == kRed && f.at(0, 1) == kGreen && f.at(0, 2) == kBlue && f.at(1, 0) == kGrey);
}

TEST(display_sprite_mosaic_and_the_second_engine_s_sprites) {
  Fixture f;
  f.simple_sprite_tile();
  for (unsigned n = 1; n < 128; ++n) f.sprite(n, kOff, 0, 0);
  f.control(0 | kSprites | kPicture);
  // Steps of two lines: line 1 shows what line 0 does.
  f.reg(DisplayEngine::kMosaic, 2, 1u << 12);
  f.sprite(0, 0 | 1u << 12, 0, kTile3Palette1);
  f.draw(2);
  CHECK(f.at(0, 0) == kRed && f.at(0, 1) == kRed && f.at(0, 2) == kGrey);

  Fixture second(DisplayEngine::kSub);
  second.simple_sprite_tile();
  for (unsigned n = 1; n < 128; ++n) second.sprite(n, kOff, 0, 0);
  second.control(0 | kSprites | kPicture);
  second.sprite(0, 0, 0, kTile3Palette1);
  second.draw();
  CHECK(second.at(0) == kRed && second.at(1) == kGreen && second.at(2) == kBlue);
}
