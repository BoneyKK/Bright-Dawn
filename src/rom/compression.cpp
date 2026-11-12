#include "rom/compression.h"

#include <cstddef>
#include <string>

namespace bd::rom {
namespace {

Error corrupt(const char* scheme, const char* what) {
  return Error{ErrorCode::Corrupt, std::string(scheme) + " stream is corrupt: " + what};
}

struct LzHeader {
  std::size_t size;      // Decompressed size.
  std::size_t data_pos;  // Offset of the first flag byte.
};

// Shared header of the forward LZ variants: a type byte, a 24-bit size, and a
// 32-bit size when the 24-bit field is zero.
Result<LzHeader> read_lz_header(ByteSpan in, std::uint8_t type, const char* scheme) {
  if (in.size() < 4) return corrupt(scheme, "missing header");
  if (in[0] != type) return corrupt(scheme, "wrong type byte");
  std::uint64_t size = le32(in, 0) >> 8;
  std::size_t pos = 4;
  if (size == 0) {
    if (in.size() < 8) return corrupt(scheme, "missing extended size");
    size = le32(in, 4);
    pos = 8;
  }
  if (size > kMaxDecompressedSize) {
    return Error{ErrorCode::Unsupported,
                 std::string(scheme) + " stream declares an implausibly large size"};
  }
  return LzHeader{static_cast<std::size_t>(size), pos};
}

// Appends `length` bytes copied from `distance` bytes behind the write
// position. Overlapping copies are legal and repeat the pattern.
bool copy_back(std::vector<std::uint8_t>& out, std::size_t size, std::size_t distance,
               std::size_t length) {
  if (distance > out.size() || length > size - out.size()) return false;
  for (std::size_t i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
  return true;
}

}  // namespace

Result<std::vector<std::uint8_t>> lz10_decompress(ByteSpan in) {
  static constexpr char kScheme[] = "LZ10";
  auto header = read_lz_header(in, 0x10, kScheme);
  if (!header) return header.error();
  const std::size_t size = header.value().size;
  std::size_t pos = header.value().data_pos;

  std::vector<std::uint8_t> out;
  out.reserve(size);
  while (out.size() < size) {
    if (pos >= in.size()) return corrupt(kScheme, "input ends early");
    const std::uint8_t flags = in[pos++];
    for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
      if ((flags >> bit) & 1) {
        if (in.size() - pos < 2) return corrupt(kScheme, "input ends inside a back-reference");
        const std::uint8_t b0 = in[pos++];
        const std::uint8_t b1 = in[pos++];
        const std::size_t length = (b0 >> 4) + 3u;
        const std::size_t distance = (((b0 & 0x0Fu) << 8) | b1) + 1u;
        if (!copy_back(out, size, distance, length))
          return corrupt(kScheme, "back-reference out of bounds");
      } else {
        if (pos >= in.size()) return corrupt(kScheme, "input ends early");
        out.push_back(in[pos++]);
      }
    }
  }
  return out;
}

Result<std::vector<std::uint8_t>> lz11_decompress(ByteSpan in) {
  static constexpr char kScheme[] = "LZ11";
  auto header = read_lz_header(in, 0x11, kScheme);
  if (!header) return header.error();
  const std::size_t size = header.value().size;
  std::size_t pos = header.value().data_pos;

  std::vector<std::uint8_t> out;
  out.reserve(size);
  while (out.size() < size) {
    if (pos >= in.size()) return corrupt(kScheme, "input ends early");
    const std::uint8_t flags = in[pos++];
    for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
      if (!((flags >> bit) & 1)) {
        if (pos >= in.size()) return corrupt(kScheme, "input ends early");
        out.push_back(in[pos++]);
        continue;
      }
      if (pos >= in.size()) return corrupt(kScheme, "input ends inside a back-reference");
      const std::uint8_t b0 = in[pos];
      // The top nibble selects one of three token widths.
      const std::size_t width = (b0 >> 4) == 0 ? 3u : (b0 >> 4) == 1 ? 4u : 2u;
      if (in.size() - pos < width) return corrupt(kScheme, "input ends inside a back-reference");
      const std::uint8_t b1 = in[pos + 1];
      std::size_t length = 0;
      std::size_t distance = 0;
      if (width == 3) {
        const std::uint8_t b2 = in[pos + 2];
        length = (((b0 & 0x0Fu) << 4) | (b1 >> 4)) + 0x11u;
        distance = (((b1 & 0x0Fu) << 8) | b2) + 1u;
      } else if (width == 4) {
        const std::uint8_t b2 = in[pos + 2];
        const std::uint8_t b3 = in[pos + 3];
        length = (((b0 & 0x0Fu) << 12) | (static_cast<std::size_t>(b1) << 4) | (b2 >> 4)) + 0x111u;
        distance = (((b2 & 0x0Fu) << 8) | b3) + 1u;
      } else {
        length = (b0 >> 4) + 1u;
        distance = (((b0 & 0x0Fu) << 8) | b1) + 1u;
      }
      pos += width;
      if (!copy_back(out, size, distance, length))
        return corrupt(kScheme, "back-reference out of bounds");
    }
  }
  return out;
}

Result<std::vector<std::uint8_t>> lz40_decompress(ByteSpan in, std::size_t* consumed,
                                                  Lz40Options options) {
  static constexpr char kScheme[] = "LZ40";
  auto header = read_lz_header(in, 0x40, kScheme);
  if (!header) return header.error();
  const std::size_t size = header.value().size;
  std::size_t pos = header.value().data_pos;

  std::vector<std::uint8_t> out;
  out.reserve(size);
  while (out.size() < size) {
    if (pos >= in.size()) return corrupt(kScheme, "input ends early");
    // Normalise to "set bit = back-reference", most significant bit first.
    std::uint32_t flags = in[pos++];
    switch (options.flags) {
      case Lz40Options::Flags::Negated: flags = (0x100u - flags) & 0xFFu; break;
      case Lz40Options::Flags::Plain: break;
      case Lz40Options::Flags::Inverted: flags ^= 0xFFu; break;
    }
    for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
      if (!((flags >> bit) & 1)) {
        if (pos >= in.size()) return corrupt(kScheme, "input ends early");
        out.push_back(in[pos++]);
        continue;
      }
      if (in.size() - pos < 2) return corrupt(kScheme, "input ends inside a back-reference");
      // A 16-bit little-endian word: distance in the top 12 bits, and a
      // length code in the low 4 that selects one of three length forms.
      const std::uint32_t word = le16(in, pos);
      pos += 2;
      const std::size_t distance = word >> 4;
      std::size_t length = word & 0x0Fu;
      if (length == 0) {
        if (pos >= in.size()) return corrupt(kScheme, "input ends inside a back-reference");
        length = in[pos++] + 0x10u;
      } else if (length == 1) {
        if (in.size() - pos < 2) return corrupt(kScheme, "input ends inside a back-reference");
        const std::uint32_t a = in[pos];
        const std::uint32_t b = in[pos + 1];
        pos += 2;
        length = (options.long_length_big_endian ? (a << 8) | b : (b << 8) | a) + 0x110u;
      }
      if (distance == 0 || !copy_back(out, size, distance, length))
        return corrupt(kScheme, "back-reference out of bounds");
    }
  }
  if (consumed != nullptr) *consumed = pos;
  return out;
}

Result<std::vector<std::uint8_t>> blz_decompress(ByteSpan in) {
  static constexpr char kScheme[] = "BLZ";
  if (in.size() < 8) return corrupt(kScheme, "missing trailer");

  // Trailer: [compressed length : 24][trailer length : 8][extra length : 32].
  // Lengths are measured back from the end of the input.
  const std::uint32_t word = le32(in, in.size() - 8);
  const std::uint64_t extra = le32(in, in.size() - 4);
  const std::size_t trailer_len = word >> 24;
  const std::size_t region_len = word & 0x00FFFFFFu;
  if (trailer_len < 8 || trailer_len > region_len || region_len > in.size())
    return corrupt(kScheme, "trailer lengths are inconsistent");

  const std::uint64_t out_size = in.size() + extra;
  if (out_size > kMaxDecompressedSize)
    return Error{ErrorCode::Unsupported, "BLZ stream declares an implausibly large size"};

  const std::size_t prefix_len = in.size() - region_len;  // Stored uncompressed.
  std::vector<std::uint8_t> out(static_cast<std::size_t>(out_size));
  for (std::size_t i = 0; i < prefix_len; ++i) out[i] = in[i];

  // Both cursors move towards the start of their buffers.
  std::size_t src = in.size() - trailer_len;
  std::size_t dst = out.size();
  while (src > prefix_len) {
    const std::uint8_t flags = in[--src];
    for (int bit = 7; bit >= 0 && src > prefix_len; --bit) {
      if (!((flags >> bit) & 1)) {
        if (dst <= prefix_len) return corrupt(kScheme, "output overruns the uncompressed prefix");
        out[--dst] = in[--src];
        continue;
      }
      if (src - prefix_len < 2) return corrupt(kScheme, "input ends inside a back-reference");
      const std::uint32_t pair = (static_cast<std::uint32_t>(in[src - 1]) << 8) | in[src - 2];
      src -= 2;
      const std::size_t length = (pair >> 12) + 3u;
      const std::size_t distance = (pair & 0x0FFFu) + 3u;
      if (length > dst - prefix_len)
        return corrupt(kScheme, "output overruns the uncompressed prefix");
      if (distance > out.size() - dst) return corrupt(kScheme, "back-reference out of bounds");
      for (std::size_t i = 0; i < length; ++i) {
        --dst;
        out[dst] = out[dst + distance];
      }
    }
  }
  if (dst != prefix_len) return corrupt(kScheme, "decoded size does not match the trailer");
  return out;
}

}  // namespace bd::rom
