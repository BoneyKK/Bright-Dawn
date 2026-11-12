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

// LZ variant with type byte 0x40, which Golden Sun: Dark Dawn applies to most
// of its assets. `consumed`, when given, receives the number of input bytes
// the stream occupied, so callers can tell padding from trailing garbage.
//
// Two details of this format are taken from a single public description and
// had not been confirmed against real data when this was written; they are
// selectable so that they can be checked (see `bdtool lz40-probe`).
struct Lz40Options {
  enum class Flags {
    Negated,   // Stored flag byte is the two's complement of the logical one.
    Plain,     // A set bit means "back-reference".
    Inverted,  // A clear bit means "back-reference".
  };
  Flags flags = Flags::Negated;
  bool long_length_big_endian = true;  // Byte order of the 16-bit length form.
};
Result<std::vector<std::uint8_t>> lz40_decompress(ByteSpan input, std::size_t* consumed = nullptr,
                                                  Lz40Options options = {});

// Backward LZ ("BLZ"), used for the ARM9 program and for overlays. The
// 8-byte trailer at the end of `input` describes the compressed region; the
// bytes before that region are stored uncompressed.
Result<std::vector<std::uint8_t>> blz_decompress(ByteSpan input);

}  // namespace bd::rom
