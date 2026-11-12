#include "rom/narc.h"

#include <cstring>
#include <utility>

#include "rom/nitrofs.h"

namespace bd::rom {
namespace {

Error corrupt(std::string what) {
  return Error{ErrorCode::Corrupt, "NARC archive is corrupt: " + std::move(what)};
}

// Finds a section by tag starting at `pos` and returns its body (without the
// 8-byte section header), advancing `pos` past the section.
Result<ByteSpan> take_section(ByteSpan data, std::size_t& pos, const char* tag) {
  if (!in_range(data.size(), pos, 8)) return corrupt(std::string("missing section ") + tag);
  if (std::memcmp(data.data() + pos, tag, 4) != 0)
    return corrupt(std::string("expected section ") + tag);
  const std::uint32_t size = le32(data, pos + 4);
  if (size < 8 || !in_range(data.size(), pos, size))
    return corrupt(std::string("section ") + tag + " has an invalid size");
  const ByteSpan body = data.subspan(pos + 8, size - 8);
  pos += size;
  return body;
}

}  // namespace

Result<Narc> Narc::parse(ByteSpan data) {
  if (data.size() < 16 || std::memcmp(data.data(), "NARC", 4) != 0)
    return corrupt("missing NARC signature");
  std::size_t pos = le16(data, 12);  // Header size; 16 in practice.
  if (pos < 16) return corrupt("header size is too small");

  // Section tags are stored byte-reversed: "BTAF" is the allocation table,
  // "BTNF" the name table and "GMIF" the file data.
  auto fat = take_section(data, pos, "BTAF");
  if (!fat) return fat.error();
  auto fnt = take_section(data, pos, "BTNF");
  if (!fnt) return fnt.error();
  auto image = take_section(data, pos, "GMIF");
  if (!image) return image.error();

  if (fat.value().size() < 4) return corrupt("allocation table is too small");
  const std::size_t count = le16(fat.value(), 0);
  if (!in_range(fat.value().size(), 4, std::uint64_t{count} * 8))
    return corrupt("allocation table is shorter than its file count");

  Narc narc;
  narc.files_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t start = le32(fat.value(), 4 + i * 8);
    const std::uint32_t end = le32(fat.value(), 8 + i * 8);
    if (start > end || end > image.value().size())
      return corrupt("member " + std::to_string(i) + " lies outside the data section");
    narc.files_.push_back(image.value().subspan(start, end - start));
  }

  narc.names_.assign(count, std::string());
  auto table = parse_name_table(fnt.value(), count);
  if (!table) return corrupt("name table: " + table.error().message);
  for (const NamedFile& file : table.value().files) narc.names_[file.file_id] = file.path;
  return narc;
}

}  // namespace bd::rom
