// Bounds-checked little-endian access to untrusted byte buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bd {

using ByteSpan = std::span<const std::uint8_t>;

// True when [offset, offset + length) lies inside a buffer of `total` bytes.
// Written so that it cannot overflow for any 64-bit inputs.
constexpr bool in_range(std::uint64_t total, std::uint64_t offset, std::uint64_t length) {
  return offset <= total && length <= total - offset;
}

inline std::optional<ByteSpan> slice(ByteSpan data, std::uint64_t offset, std::uint64_t length) {
  if (!in_range(data.size(), offset, length)) return std::nullopt;
  return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

// Unchecked reads: the caller has already established that the range is valid.
inline std::uint16_t le16(ByteSpan data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

inline std::uint32_t le32(ByteSpan data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

}  // namespace bd
