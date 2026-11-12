#include "rom/nitrofs.h"

#include <algorithm>
#include <utility>

namespace bd::rom {
namespace {

// File IDs share a 16-bit space with directory IDs, which start at 0xF000.
constexpr std::uint32_t kFirstDirectoryId = 0xF000;
constexpr std::uint32_t kMaxDirectories = 0x1000;

Error corrupt(std::string what) {
  return Error{ErrorCode::Corrupt, "file system is corrupt: " + std::move(what)};
}

}  // namespace

Result<NameTable> parse_name_table(ByteSpan fnt, std::size_t file_count) {
  NameTable table;
  if (fnt.size() < 8) return corrupt("name table is too small");

  // The root entry stores the directory count where others store their parent.
  const std::uint32_t dir_count = le16(fnt, 6);
  if (dir_count == 0 || dir_count > kMaxDirectories || std::uint64_t{dir_count} * 8 > fnt.size())
    return corrupt("name table declares an impossible number of directories");
  table.directory_count = dir_count;

  struct Pending {
    std::uint32_t index;
    std::string prefix;
  };
  std::vector<Pending> stack;
  stack.push_back(Pending{0, ""});
  // Each directory may be entered once; this also rules out cycles.
  std::vector<bool> visited(dir_count, false);
  std::vector<bool> named(file_count, false);

  while (!stack.empty()) {
    Pending dir = std::move(stack.back());
    stack.pop_back();
    if (visited[dir.index]) return corrupt("a directory is referenced more than once");
    visited[dir.index] = true;

    std::uint64_t pos = le32(fnt, dir.index * 8);
    std::uint32_t file_id = le16(fnt, dir.index * 8 + 4);
    for (;;) {
      if (pos >= fnt.size()) return corrupt("directory listing runs past the name table");
      const std::uint8_t type = fnt[static_cast<std::size_t>(pos++)];
      if (type == 0x00) break;
      if (type == 0x80) return corrupt("reserved entry type in a directory listing");
      const std::size_t name_len = type & 0x7F;
      const bool is_directory = (type & 0x80) != 0;
      if (!in_range(fnt.size(), pos, name_len + (is_directory ? 2u : 0u)))
        return corrupt("entry name runs past the name table");
      std::string name(reinterpret_cast<const char*>(fnt.data() + pos), name_len);
      pos += name_len;

      if (is_directory) {
        const std::uint32_t id = le16(fnt, static_cast<std::size_t>(pos));
        pos += 2;
        if (id < kFirstDirectoryId || id - kFirstDirectoryId >= dir_count)
          return corrupt("entry refers to a directory that does not exist");
        stack.push_back(Pending{id - kFirstDirectoryId, dir.prefix + name + "/"});
      } else {
        if (file_id >= file_count)
          return corrupt("named file has no entry in the allocation table");
        if (named[file_id]) return corrupt("a file ID has more than one name");
        named[file_id] = true;
        table.files.push_back(NamedFile{dir.prefix + name, static_cast<std::uint16_t>(file_id)});
        ++file_id;
      }
    }
  }
  return table;
}

Result<NitroFs> NitroFs::parse(ByteSpan rom, const NdsHeader& header) {
  NitroFs fs;

  // --- File allocation table ---
  if (header.fat.size % 8 != 0) return corrupt("allocation table size is not a multiple of 8");
  const auto fat = slice(rom, header.fat.offset, header.fat.size);
  if (!fat) return Error{ErrorCode::Truncated, "file allocation table lies outside the image"};
  const std::size_t file_count = fat->size() / 8;
  if (file_count > kFirstDirectoryId) return corrupt("allocation table has too many entries");

  fs.ranges_.reserve(file_count);
  for (std::size_t i = 0; i < file_count; ++i) {
    const std::uint32_t start = le32(*fat, i * 8);
    const std::uint32_t end = le32(*fat, i * 8 + 4);
    if (start > end) return corrupt("file " + std::to_string(i) + " ends before it starts");
    if (end > rom.size()) {
      return Error{ErrorCode::Truncated,
                   "file " + std::to_string(i) + " extends past the end of the image"};
    }
    fs.ranges_.push_back(FileRange{start, end - start});
  }
  fs.name_index_.assign(file_count, -1);

  // --- File name table ---
  if (header.fnt.size == 0) return fs;
  const auto fnt = slice(rom, header.fnt.offset, header.fnt.size);
  if (!fnt) return Error{ErrorCode::Truncated, "file name table lies outside the image"};
  auto table = parse_name_table(*fnt, file_count);
  if (!table) return table.error();
  fs.directory_count_ = table.value().directory_count;
  fs.named_ = std::move(table.value().files);

  std::sort(fs.named_.begin(), fs.named_.end(),
            [](const NamedFile& x, const NamedFile& y) { return x.file_id < y.file_id; });
  for (std::size_t i = 0; i < fs.named_.size(); ++i) {
    fs.name_index_[fs.named_[i].file_id] = static_cast<std::int32_t>(i);
    // Duplicate paths are tolerated; lookups resolve to the lowest file ID.
    fs.by_path_.emplace(fs.named_[i].path, i);
  }
  return fs;
}

const NamedFile* NitroFs::find(std::string_view path) const {
  const auto it = by_path_.find(std::string(path));
  return it == by_path_.end() ? nullptr : &named_[it->second];
}

const std::string* NitroFs::path_of(std::uint16_t file_id) const {
  if (file_id >= name_index_.size() || name_index_[file_id] < 0) return nullptr;
  return &named_[static_cast<std::size_t>(name_index_[file_id])].path;
}

}  // namespace bd::rom
