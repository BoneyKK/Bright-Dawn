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
  }
  return "";
}

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
  return {};
}

}  // namespace bd::rom
