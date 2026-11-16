// Screen pictures (assets/screen_picture.h), from pictures made up here.
#include <cstdint>
#include <vector>

#include "assets/screen_picture.h"
#include "codecs.h"
#include "rom/rom.h"
#include "rom_builder.h"
#include "test.h"

namespace {

using bd_test::Bytes;
constexpr std::size_t kPixels = 256 * 192;

// A palette of `colours`, then pixels that go through them in turn.
Bytes made_up_picture(const std::vector<std::uint16_t>& colours) {
  Bytes file;
  for (std::uint16_t c : colours) {
    file.push_back(static_cast<std::uint8_t>(c));
    file.push_back(static_cast<std::uint8_t>(c >> 8));
  }
  for (std::size_t i = 0; i < kPixels; ++i) file.push_back(static_cast<std::uint8_t>((i / 7) % colours.size()));
  return file;
}

bool pixel_is(const bd::assets::Picture& p, std::size_t i, int r, int g, int b) {
  return p.rgba[i * 4] == r && p.rgba[i * 4 + 1] == g && p.rgba[i * 4 + 2] == b && p.rgba[i * 4 + 3] == 255;
}

}  // namespace

TEST(screen_picture_colours_widen_to_full_range) {
  CHECK(bd::assets::widen5(0) == 0);
  CHECK(bd::assets::widen5(31) == 255);
  CHECK(bd::assets::widen5(16) == 132);
}

TEST(screen_picture_decodes_stored_and_compressed) {
  // Black, red, green, blue, white: red is the lowest five bits.
  const Bytes raw = made_up_picture({0x0000, 0x001F, 0x03E0, 0x7C00, 0x7FFF});
  for (const Bytes& file : {raw, bd_test::lz40_compress(raw)}) {
    auto picture = bd::assets::decode_screen_picture(file);
    CHECK(picture.ok());
    if (!picture) continue;
    const auto& p = picture.value();
    CHECK(p.width == 256 && p.height == 192 && p.rgba.size() == kPixels * 4);
    CHECK(pixel_is(p, 0, 0, 0, 0));
    CHECK(pixel_is(p, 7, 255, 0, 0));
    CHECK(pixel_is(p, 14, 0, 255, 0));
    CHECK(pixel_is(p, 21, 0, 0, 255));
    CHECK(pixel_is(p, 28, 255, 255, 255));
    CHECK(pixel_is(p, kPixels - 1, 255, 0, 0));  // Colour (49151 / 7) % 5 = 1.
  }
}

TEST(screen_picture_takes_a_separate_palette) {
  Bytes pixels(kPixels, 1);
  const Bytes palette = {0x00, 0x00, 0x1F, 0x00};
  auto picture = bd::assets::decode_screen_picture(pixels, bd::ByteSpan(palette));
  CHECK(picture.ok());
  if (picture) CHECK(pixel_is(picture.value(), 1234, 255, 0, 0));
  // A file with its own palette and another given: not what it should be.
  CHECK(!bd::assets::decode_screen_picture(made_up_picture({0, 1}), bd::ByteSpan(palette)).ok());
}

TEST(screen_picture_refuses_what_is_not_one) {
  CHECK(!bd::assets::decode_screen_picture(Bytes(100, 0)).ok());          // Too small.
  CHECK(!bd::assets::decode_screen_picture(Bytes(kPixels, 0)).ok());      // No palette.
  CHECK(!bd::assets::decode_screen_picture(Bytes(kPixels + 3, 0)).ok());  // Half a colour.
  CHECK(!bd::assets::decode_screen_picture(Bytes(kPixels + 514, 0)).ok());  // Over 256 colours.
  Bytes beyond = made_up_picture({0, 1, 2});
  beyond.back() = 3;  // A pixel names a colour the palette does not have.
  auto picture = bd::assets::decode_screen_picture(beyond);
  CHECK(!picture.ok());
  if (!picture) CHECK(picture.error().message.find("colour 3 of 3") != std::string::npos);
}

TEST(screen_picture_is_read_from_the_rom) {
  bd_test::RomSpec spec = bd_test::default_spec();
  spec.files.emplace_back("data/upscreen/page.bin", bd_test::lz40_compress(made_up_picture({0x7FFF, 0x001F})));
  auto rom = bd::rom::Rom::from_bytes(bd_test::build_rom(spec).image);
  CHECK(rom.ok());
  if (!rom) return;
  auto picture = bd::assets::read_screen_picture(rom.value(), "data/upscreen/page.bin");
  CHECK(picture.ok());
  if (picture) CHECK(pixel_is(picture.value(), 7, 255, 0, 0));
  auto missing = bd::assets::read_screen_picture(rom.value(), "data/upscreen/none.bin");
  CHECK(!missing.ok());
  if (!missing) CHECK(missing.error().code == bd::ErrorCode::NotFound);
}

TEST(menu_piece_of_another_size_is_decoded_with_colour_zero_clear) {
  // A palette of two, then 4 by 2 pixels: colour 0 and colour 1 in turn.
  const Bytes raw = {0x00, 0x00, 0x1F, 0x00, 0, 1, 0, 1, 1, 0, 1, 0};
  for (const Bytes& file : {raw, bd_test::lz40_compress(raw)}) {
    auto clear = bd::assets::decode_indexed_picture(file, 4, 2, true);
    CHECK(clear.ok());
    if (!clear) continue;
    const auto& p = clear.value();
    CHECK(p.width == 4 && p.height == 2 && p.rgba.size() == 4 * 2 * 4);
    CHECK(p.rgba[3] == 0);              // Colour 0: clear.
    CHECK(pixel_is(p, 1, 255, 0, 0));  // Colour 1: red, solid.
    CHECK(p.rgba[4 * 4 + 3] == 255 && pixel_is(p, 4, 255, 0, 0));
  }
  auto solid = bd::assets::decode_indexed_picture(raw, 4, 2, false);
  CHECK(solid.ok());
  if (solid) CHECK(pixel_is(solid.value(), 0, 0, 0, 0));
  // Too few bytes for the size and a palette, or a size that cannot be.
  CHECK(!bd::assets::decode_indexed_picture(raw, 5, 2, true).ok());
  CHECK(!bd::assets::decode_indexed_picture(raw, 0, 2, true).ok());
  CHECK(!bd::assets::decode_indexed_picture(raw, 4, 4096, true).ok());
}
