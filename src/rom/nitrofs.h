// The cartridge file system ("NitroFS"): a file allocation table that maps
// numeric file IDs to byte ranges, and a name table that arranges some of
// those IDs into a directory tree. Overlay files normally have an ID but no
// name.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/nds_header.h"

namespace bd::rom {

struct FileRange {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

struct NamedFile {
  std::string path;  // '/'-separated, no leading slash, raw bytes from the ROM.
  std::uint16_t file_id = 0;
};

struct NameTable {
  std::vector<NamedFile> files;  // In the order the directory tree lists them.
  std::size_t directory_count = 0;
};

// Parses a file name table, the structure shared by the cartridge file system
// and by NARC archives. `file_count` is the number of files that exist; names
// that refer to any other file ID are rejected.
Result<NameTable> parse_name_table(ByteSpan fnt, std::size_t file_count);

class NitroFs {
 public:
  // Reads both tables from `rom`. Every structural problem is reported as an
  // error; a successfully parsed file system only refers to bytes inside `rom`.
  static Result<NitroFs> parse(ByteSpan rom, const NdsHeader& header);

  // All FAT entries, indexed by file ID.
  const std::vector<FileRange>& ranges() const { return ranges_; }
  // Files that appear in the directory tree, ordered by file ID.
  const std::vector<NamedFile>& named_files() const { return named_; }
  std::size_t directory_count() const { return directory_count_; }

  // Looks a file up by its full path. Returns nullptr when there is none.
  const NamedFile* find(std::string_view path) const;
  // Path of a file ID, or nullptr when the ID has no name.
  const std::string* path_of(std::uint16_t file_id) const;

 private:
  std::vector<FileRange> ranges_;
  std::vector<NamedFile> named_;
  std::unordered_map<std::string, std::size_t> by_path_;
  std::vector<std::int32_t> name_index_;  // Per file ID: index into named_, or -1.
  std::size_t directory_count_ = 0;
};

}  // namespace bd::rom
