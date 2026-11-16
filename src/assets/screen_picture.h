// Pictures the size of a DS screen, as the game keeps the backgrounds of its
// menus: a palette of up to 256 colours (two bytes each, 5 bits of red,
// green and blue, red lowest), then 256 by 192 bytes, each the colour of a
// pixel, row after row from the top left. The file is usually LZ40
// compressed as a whole. Some files have no palette of their own; their
// colours are in another file, which `palette` then gives.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/rom.h"

namespace bd::assets {

inline constexpr int kScreenPictureWidth = 256;
inline constexpr int kScreenPictureHeight = 192;

// An image decoded: four bytes a pixel (red, green, blue, alpha), row after
// row from the top left.
struct Picture {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

// A DS colour (5 bits each of red, green and blue, red lowest) as 8 bits each,
// the top bits repeated in the low ones so that full is 255.
inline std::uint8_t widen5(unsigned value) {
  value &= 31u;
  return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

// Decodes a screen picture. Fails when the file is not one: its size does
// not fit, or a pixel names a colour the palette does not have.
Result<Picture> decode_screen_picture(ByteSpan file, std::optional<ByteSpan> palette = std::nullopt);

// The screen picture at `path` in the ROM's file system.
Result<Picture> read_screen_picture(const rom::Rom& rom, std::string_view path);

}  // namespace bd::assets
