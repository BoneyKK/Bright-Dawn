// A DS screen as the console model makes it, turned into the bytes a
// window system takes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::gfx {

// One part of a colour of six bits (0-63) made eight (0-255), so that 0 stays
// black and 63 becomes full brightness.
constexpr std::uint8_t eight_bits(std::uint32_t six) {
  return static_cast<std::uint8_t>((six & 0x3F) << 2 | (six & 0x3F) >> 4);
}

// `count` pixels of six bits a part (red in bits 0-5, green 6-11, blue 12-17;
// anything above is ignored) into four bytes each: red, green, blue, and 255.
void to_rgba_bytes(const std::uint32_t* pixels, std::size_t count, std::uint8_t* out);

}  // namespace bd::gfx
