// NARC: the generic archive format of the Nintendo DS SDK. It is a small
// file system of its own, with an allocation table, an optional name table
// and a data block. Golden Sun: Dark Dawn stores most of its assets in NARC
// archives (also under the extension ".cbarc").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::rom {

class Narc {
 public:
  // Parses an archive. The result refers to `data` and must not outlive it.
  static Result<Narc> parse(ByteSpan data);

  std::size_t file_count() const { return files_.size(); }
  ByteSpan file(std::size_t index) const { return files_[index]; }
  // Path of a member inside the archive, or an empty string when the archive
  // stores no name for it (many archives are addressed by index only).
  const std::string& name(std::size_t index) const { return names_[index]; }

 private:
  std::vector<ByteSpan> files_;
  std::vector<std::string> names_;
};

}  // namespace bd::rom
