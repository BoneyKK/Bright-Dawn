// Reference encoders used only by the tests, so that the project's decoders
// can be exercised on realistic streams without any game data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bd_test {

using Bytes = std::vector<std::uint8_t>;

Bytes lz10_compress(const Bytes& raw);
Bytes lz11_compress(const Bytes& raw);

// Backward LZ. The first `keep_prefix` bytes are stored uncompressed, as the
// SDK does for the start-up code that performs the decompression.
Bytes blz_compress(const Bytes& raw, std::size_t keep_prefix);

// Deterministic data that compresses well but not trivially.
Bytes sample_data(std::size_t size, std::uint32_t seed);

}  // namespace bd_test
