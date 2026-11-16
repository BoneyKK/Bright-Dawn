#include "gfx/screen_image.h"

namespace bd::gfx {

void to_rgba_bytes(const std::uint32_t* pixels, std::size_t count, std::uint8_t* out) {
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t p = pixels[i];
    out[4 * i + 0] = eight_bits(p);
    out[4 * i + 1] = eight_bits(p >> 6);
    out[4 * i + 2] = eight_bits(p >> 12);
    out[4 * i + 3] = 255;
  }
}

}  // namespace bd::gfx
