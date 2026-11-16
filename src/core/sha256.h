// SHA-256 (FIPS 180-4), for checking what is downloaded against the hash it
// is known by.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "core/bytes.h"

namespace bd {

class Sha256 {
 public:
  Sha256();
  void update(const std::uint8_t* data, std::size_t size);
  void update(ByteSpan data) { update(data.data(), data.size()); }
  // The digest; the hash cannot be updated after.
  std::array<std::uint8_t, 32> finish();

 private:
  void block(const std::uint8_t* p);
  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;  // In bytes.
};

// The digest as 64 lowercase hexadecimal digits.
std::string to_hex(const std::array<std::uint8_t, 32>& digest);
// The SHA-256 of a file's contents, in hexadecimal; nothing if it cannot be read.
std::optional<std::string> sha256_of_file(const std::filesystem::path& path);

}  // namespace bd
