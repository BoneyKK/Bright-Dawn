// Identification of file formats by content. This is what lets us survey an
// unfamiliar ROM and learn which of its assets use documented Nintendo
// formats and which use formats specific to the game.
#pragma once

#include <cstdint>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::rom {

enum class FileFormat {
  Unknown,
  Empty,
  Sdat,   // Sound archive (sequences, instrument banks, wave data, streams).
  Sseq,   // Music sequence.
  Ssar,   // Sequence archive (sound effects).
  Sbnk,   // Instrument bank.
  Swar,   // Wave archive.
  Strm,   // Streamed audio.
  Narc,   // Generic file archive.
  Nsbmd,  // 3D model.
  Nsbtx,  // 3D texture set.
  Nsbca,  // Skeletal animation.
  Nsbta,  // Texture coordinate animation.
  Nsbtp,  // Texture pattern animation.
  Nsbma,  // Material animation.
  Nsbva,  // Visibility animation.
  Nclr,   // 2D palette.
  Ncgr,   // 2D tile graphics.
  Nscr,   // 2D tile map.
  Ncer,   // 2D sprite cell layout.
  Nanr,   // 2D sprite animation.
  Nftr,   // Bitmap font.
  Lz10,   // LZ10-compressed data.
  Lz11,   // LZ11-compressed data.
  Lz40,   // LZ40-compressed data (the scheme this game uses throughout).
  Lz40Unconfirmed,  // Has a plausible 0x40 header but did not decode.
};

inline constexpr FileFormat kLastFileFormat = FileFormat::Lz40Unconfirmed;

const char* name(FileFormat format);
const char* description(FileFormat format);

struct Sniffed {
  FileFormat format = FileFormat::Unknown;
  // For compressed files: the format of the decompressed content.
  FileFormat inner = FileFormat::Unknown;
};

// Identifies `data` by its leading bytes. A file is only reported as
// compressed when it actually decompresses (and, for the 0x40 scheme, when
// the stream accounts for the whole file), which keeps false positives rare.
// Only the outermost layer is examined; files may be compressed more than once.
Sniffed sniff(ByteSpan data);

// True for the formats that wrap other data in compression.
bool is_compression(FileFormat format);

// Removes the compression `format` names. Fails with InvalidArgument for a
// format that is not a compression scheme.
Result<std::vector<std::uint8_t>> decompress(FileFormat format, ByteSpan data);

}  // namespace bd::rom
