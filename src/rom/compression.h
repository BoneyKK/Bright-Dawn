// Decoders for the compression schemes the Nintendo DS system software
// provides. All of them treat their input as untrusted.
#pragma once

#include <cstddef>
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

// LZ variant with type byte 0x40 ("LZ40"), which Golden Sun: Dark Dawn applies
// to almost every file, including its overlays. The console's built-in
// routines do not handle it; the game carries its own decoder.
//
// Layout: the usual type byte and 24-bit size, then flag bytes stored as
// their two's complement (most significant bit first, set = back-reference).
// A back-reference is a little-endian 16-bit word with the distance in the
// top 12 bits and a length code in the low 4: codes 2-15 are the length
// itself, 0 takes one more byte (+16), 1 takes a little-endian 16-bit value
// (+272). The distance is used as is. Output past the declared size is cut.
//
// `consumed`, when given, receives the number of input bytes the stream used.
Result<std::vector<std::uint8_t>> lz40_decompress(ByteSpan input, std::size_t* consumed = nullptr);

// Files are padded after the compressed stream; up to this many bytes are
// accepted. Anything longer means the stream is not the whole file.
inline constexpr std::size_t kLz40MaxPadding = 32;

// Decodes a file that is LZ40-compressed as a whole: as lz40_decompress, but
// fails unless the stream ends within kLz40MaxPadding bytes of the end.
Result<std::vector<std::uint8_t>> lz40_decompress_file(ByteSpan input);

// Backward LZ ("BLZ"), used for the ARM9 program and for overlays. The
// 8-byte trailer at the end of `input` describes the compressed region; the
// bytes before that region are stored uncompressed.
Result<std::vector<std::uint8_t>> blz_decompress(ByteSpan input);

}  // namespace bd::rom
