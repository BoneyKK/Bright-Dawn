#include "rom/sdat.h"

#include <cstring>
#include <optional>

namespace bd::rom {
namespace {

// The archive's header: magic, byte order, version, size, header size,
// block count, then offset and size of SYMB, INFO, FAT and FILE.
constexpr std::size_t kBlocksAt = 0x10;
constexpr std::size_t kHeaderSize = 0x40;
// In SYMB and INFO: the magic and size, then the offsets of eight records
// (sequences first), from the block's start.
constexpr std::size_t kRecordsAt = 0x08;
constexpr unsigned kSequenceRecord = 0;
// A sequence's entry in INFO.
constexpr std::size_t kSequenceInfoSize = 12;
// A file's entry in FAT: offset from the archive's start, size, 8 reserved.
constexpr std::size_t kFatEntriesAt = 0x0C;
constexpr std::size_t kFatEntrySize = 16;
// An SSEQ file: its header (0x10 bytes), then the DATA block, whose third
// word is where the events start, from the file's start.
constexpr std::size_t kSseqDataOffsetAt = 0x18;

Error corrupt(const std::string& what) { return Error{ErrorCode::Corrupt, "sound archive: " + what}; }

bool magic(ByteSpan data, std::size_t at, const char* expected) {
  return in_range(data.size(), at, 4) && std::memcmp(data.data() + at, expected, 4) == 0;
}

std::optional<std::uint32_t> word(ByteSpan data, std::uint64_t at) {
  if (!in_range(data.size(), at, 4)) return std::nullopt;
  return le32(data, static_cast<std::size_t>(at));
}

// A block named by its offset and size in the header, checked to be inside
// the archive and to start with its magic.
std::optional<ByteSpan> block(ByteSpan sdat, unsigned n, const char* name) {
  const auto offset = word(sdat, kBlocksAt + n * 8);
  const auto size = word(sdat, kBlocksAt + n * 8 + 4);
  if (!offset || !size || *offset == 0) return std::nullopt;
  const auto b = slice(sdat, *offset, *size);
  if (!b || !magic(*b, 0, name)) return std::nullopt;
  return b;
}

// The record's list: its count, then that many offsets from the block's start.
std::optional<std::vector<std::uint32_t>> record_list(ByteSpan b, unsigned record) {
  const auto at = word(b, kRecordsAt + record * 4);
  if (!at) return std::nullopt;
  const auto count = word(b, *at);
  if (!count || !in_range(b.size(), *at + 4ull, static_cast<std::uint64_t>(*count) * 4)) return std::nullopt;
  std::vector<std::uint32_t> list(*count);
  for (std::uint32_t i = 0; i < *count; ++i) list[i] = le32(b, *at + 4 + i * 4);
  return list;
}

std::string name_at(ByteSpan symb, std::uint32_t at) {
  std::string name;
  for (std::size_t i = at; at != 0 && i < symb.size() && symb[i] != 0 && name.size() < 256; ++i)
    name += static_cast<char>(symb[i]);
  return name;
}

}  // namespace

Result<std::vector<SdatSequence>> sdat_sequences(ByteSpan sdat) {
  if (!magic(sdat, 0, "SDAT") || sdat.size() < kHeaderSize) return corrupt("no SDAT header");
  const auto info = block(sdat, 1, "INFO");
  const auto fat = block(sdat, 2, "FAT ");
  if (!info || !fat) return corrupt("its INFO or FAT block is missing or out of bounds");
  const auto entries = record_list(*info, kSequenceRecord);
  if (!entries) return corrupt("its list of sequences is out of bounds");
  const auto files = word(*fat, 8);
  if (!files || !in_range(fat->size(), kFatEntriesAt, static_cast<std::uint64_t>(*files) * kFatEntrySize))
    return corrupt("its file table is out of bounds");
  // Names are optional; a damaged name table only loses the names.
  std::vector<std::uint32_t> names;
  std::optional<ByteSpan> symb = block(sdat, 0, "SYMB");
  if (symb) names = record_list(*symb, kSequenceRecord).value_or(std::vector<std::uint32_t>{});

  std::vector<SdatSequence> out;
  for (std::size_t i = 0; i < entries->size(); ++i) {
    const std::uint32_t at = (*entries)[i];
    if (at == 0) continue;
    if (!in_range(info->size(), at, kSequenceInfoSize)) return corrupt("sequence " + std::to_string(i) + " is out of bounds");
    SdatSequence s;
    s.index = static_cast<unsigned>(i);
    const std::uint16_t file = le16(*info, at);
    s.bank = le16(*info, at + 4);
    s.volume = (*info)[at + 6];
    s.player = (*info)[at + 9];
    if (file >= *files) return corrupt("sequence " + std::to_string(i) + " names a file past the table");
    const std::size_t entry = kFatEntriesAt + file * kFatEntrySize;
    const auto sseq = slice(sdat, le32(*fat, entry), le32(*fat, entry + 4));
    if (!sseq || !magic(*sseq, 0, "SSEQ")) return corrupt("sequence " + std::to_string(i) + "'s file is not an SSEQ");
    const auto data_at = word(*sseq, kSseqDataOffsetAt);
    if (!data_at || *data_at > sseq->size()) return corrupt("sequence " + std::to_string(i) + "'s events are out of bounds");
    s.data = sseq->subspan(*data_at);
    if (symb && i < names.size()) s.name = name_at(*symb, names[i]);
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace bd::rom
