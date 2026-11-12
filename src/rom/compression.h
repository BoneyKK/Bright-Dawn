// Decoders for the compression schemes the Nintendo DS system software
// provides. All of them treat their input as untrusted.
#pragma once

#include <cstdint>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::rom {

// Upper bound accepted for any single decompressed buffer. A DS has 4 MiB of
// main memory, so anything near this limit is certainly not genuine.
inline constexpr std::uint64_t kMaxDecompressedSize = 64ull * 1024 * 1024;

// LZ77 variant with type byte 0x10 ("LZ10").
Result<std::vector<std::uint8_t>> lz10_decompress(ByteSpan input);

// Extended LZ77 variant with type byte 0x11 ("LZ11").
Result<std::vector<std::uint8_t>> lz11_decompress(ByteSpan input);

// Backward LZ ("BLZ"), used for the ARM9 program and for overlays. The
// 8-byte trailer at the end of `input` describes the compressed region; the
// bytes before that region are stored uncompressed.
Result<std::vector<std::uint8_t>> blz_decompress(ByteSpan input);

}  // namespace bd::rom
