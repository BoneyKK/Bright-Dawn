// Reference encoders used only by the tests, so that the project's decoders
// can be exercised on realistic streams without any game data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bd_test {

using Bytes = std::vector<std::uint8_t>;

Bytes lz10_compress(const Bytes& raw);
Bytes lz11_compress(const Bytes& raw);

// The game's 0x40 scheme. `negated_flags` and `long_length_big_endian` select
// between the readings of the format that the decoder can be told to use.
enum class Lz40Flags { Negated, Plain, Inverted };
Bytes lz40_compress(const Bytes& raw, Lz40Flags flags = Lz40Flags::Negated,
                    bool long_length_big_endian = true);

// A NARC archive. Members with an empty name are left out of the name table.
struct NarcMember {
  std::string name;
  Bytes data;
};
Bytes build_narc(const std::vector<NarcMember>& members);

// Backward LZ. The first `keep_prefix` bytes are stored uncompressed, as the
// SDK does for the start-up code that performs the decompression.
Bytes blz_compress(const Bytes& raw, std::size_t keep_prefix);

// Deterministic data that compresses well but not trivially.
Bytes sample_data(std::size_t size, std::uint32_t seed);

}  // namespace bd_test
