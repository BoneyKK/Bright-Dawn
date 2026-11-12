#include "rom/file_format.h"

#include <cstring>

#include "rom/compression.h"

namespace bd::rom {
namespace {

struct Magic {
  char tag[5];
  FileFormat format;
};

// 2D formats store their tag byte-reversed ("RLCN" for an NCLR file).
constexpr Magic kMagics[] = {
    {"SDAT", FileFormat::Sdat},  {"SSEQ", FileFormat::Sseq},  {"SSAR", FileFormat::Ssar},
    {"SBNK", FileFormat::Sbnk},  {"SWAR", FileFormat::Swar},  {"STRM", FileFormat::Strm},
    {"NARC", FileFormat::Narc},  {"BMD0", FileFormat::Nsbmd}, {"BTX0", FileFormat::Nsbtx},
    {"BCA0", FileFormat::Nsbca}, {"BTA0", FileFormat::Nsbta}, {"BTP0", FileFormat::Nsbtp},
    {"BMA0", FileFormat::Nsbma}, {"BVA0", FileFormat::Nsbva}, {"RLCN", FileFormat::Nclr},
    {"RGCN", FileFormat::Ncgr},  {"RCSN", FileFormat::Nscr},  {"RECN", FileFormat::Ncer},
    {"RNAN", FileFormat::Nanr},  {"RTFN", FileFormat::Nftr},
};

FileFormat by_magic(ByteSpan data) {
  if (data.size() < 4) return FileFormat::Unknown;
  for (const Magic& magic : kMagics) {
    if (std::memcmp(data.data(), magic.tag, 4) == 0) return magic.format;
  }
  return FileFormat::Unknown;
}

}  // namespace

const char* name(FileFormat format) {
  switch (format) {
    case FileFormat::Unknown: return "unknown";
    case FileFormat::Empty: return "empty";
    case FileFormat::Sdat: return "SDAT";
    case FileFormat::Sseq: return "SSEQ";
    case FileFormat::Ssar: return "SSAR";
    case FileFormat::Sbnk: return "SBNK";
    case FileFormat::Swar: return "SWAR";
    case FileFormat::Strm: return "STRM";
    case FileFormat::Narc: return "NARC";
    case FileFormat::Nsbmd: return "NSBMD";
    case FileFormat::Nsbtx: return "NSBTX";
    case FileFormat::Nsbca: return "NSBCA";
    case FileFormat::Nsbta: return "NSBTA";
    case FileFormat::Nsbtp: return "NSBTP";
    case FileFormat::Nsbma: return "NSBMA";
    case FileFormat::Nsbva: return "NSBVA";
    case FileFormat::Nclr: return "NCLR";
    case FileFormat::Ncgr: return "NCGR";
    case FileFormat::Nscr: return "NSCR";
    case FileFormat::Ncer: return "NCER";
    case FileFormat::Nanr: return "NANR";
    case FileFormat::Nftr: return "NFTR";
    case FileFormat::Lz10: return "LZ10";
    case FileFormat::Lz11: return "LZ11";
    case FileFormat::Lz40: return "LZ40";
    case FileFormat::Lz40Unconfirmed: return "0x40?";
  }
  return "unknown";
}

const char* description(FileFormat format) {
  switch (format) {
    case FileFormat::Unknown: return "unrecognised (possibly game-specific)";
    case FileFormat::Empty: return "zero-length file";
    case FileFormat::Sdat: return "sound archive";
    case FileFormat::Sseq: return "music sequence";
    case FileFormat::Ssar: return "sequence archive";
    case FileFormat::Sbnk: return "instrument bank";
    case FileFormat::Swar: return "wave archive";
    case FileFormat::Strm: return "streamed audio";
    case FileFormat::Narc: return "file archive";
    case FileFormat::Nsbmd: return "3D model";
    case FileFormat::Nsbtx: return "3D texture set";
    case FileFormat::Nsbca: return "skeletal animation";
    case FileFormat::Nsbta: return "texture coordinate animation";
    case FileFormat::Nsbtp: return "texture pattern animation";
    case FileFormat::Nsbma: return "material animation";
    case FileFormat::Nsbva: return "visibility animation";
    case FileFormat::Nclr: return "2D palette";
    case FileFormat::Ncgr: return "2D tile graphics";
    case FileFormat::Nscr: return "2D tile map";
    case FileFormat::Ncer: return "2D sprite cells";
    case FileFormat::Nanr: return "2D sprite animation";
    case FileFormat::Nftr: return "bitmap font";
    case FileFormat::Lz10: return "LZ10-compressed data";
    case FileFormat::Lz11: return "LZ11-compressed data";
    case FileFormat::Lz40: return "0x40-compressed data";
    case FileFormat::Lz40Unconfirmed: return "has a 0x40 compression header but did not decode";
  }
  return "";
}

namespace {

// Files are padded to a 4-byte boundary, so up to three bytes may follow a
// compressed stream. More than that means the stream was not the whole file.
constexpr std::size_t kMaxPadding = 3;

// Whether the header alone is believable: a non-zero size that the file could
// actually expand to (literal-only data costs nine bytes per eight).
bool plausible_lz40_header(ByteSpan data) {
  if (data.size() < 5 || data[0] != 0x40) return false;
  const std::uint64_t declared = le32(data, 0) >> 8;
  return declared != 0 && data.size() <= declared + declared / 8 + 8;
}

}  // namespace

Sniffed sniff(ByteSpan data) {
  if (data.empty()) return {FileFormat::Empty, FileFormat::Unknown};
  if (const FileFormat format = by_magic(data); format != FileFormat::Unknown)
    return {format, FileFormat::Unknown};

  if (data.size() > 4 && (data[0] == 0x10 || data[0] == 0x11)) {
    const bool is_lz10 = data[0] == 0x10;
    const auto decoded = is_lz10 ? lz10_decompress(data) : lz11_decompress(data);
    if (decoded && !decoded.value().empty()) {
      return {is_lz10 ? FileFormat::Lz10 : FileFormat::Lz11, by_magic(decoded.value())};
    }
  }
  if (plausible_lz40_header(data)) {
    std::size_t consumed = 0;
    const auto decoded = lz40_decompress(data, &consumed);
    if (decoded && data.size() - consumed <= kMaxPadding)
      return {FileFormat::Lz40, by_magic(decoded.value())};
    return {FileFormat::Lz40Unconfirmed, FileFormat::Unknown};
  }
  return {};
}

bool is_compression(FileFormat format) {
  return format == FileFormat::Lz10 || format == FileFormat::Lz11 || format == FileFormat::Lz40;
}

Result<std::vector<std::uint8_t>> decompress(FileFormat format, ByteSpan data) {
  switch (format) {
    case FileFormat::Lz10: return lz10_decompress(data);
    case FileFormat::Lz11: return lz11_decompress(data);
    case FileFormat::Lz40: return lz40_decompress(data);
    default: return Error{ErrorCode::InvalidArgument, "not a compression format"};
  }
}

}  // namespace bd::rom
