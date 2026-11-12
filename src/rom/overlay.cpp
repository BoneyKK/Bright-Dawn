#include "rom/overlay.h"

#include <string>

namespace bd::rom {

Result<std::vector<Overlay>> parse_overlay_table(ByteSpan rom, TableRef table,
                                                 std::size_t file_count) {
  constexpr std::size_t kEntrySize = 32;
  std::vector<Overlay> overlays;
  if (table.size == 0) return overlays;
  if (table.size % kEntrySize != 0)
    return Error{ErrorCode::Corrupt, "overlay table size is not a multiple of 32"};
  const auto data = slice(rom, table.offset, table.size);
  if (!data) return Error{ErrorCode::Truncated, "overlay table lies outside the image"};

  overlays.reserve(data->size() / kEntrySize);
  for (std::size_t pos = 0; pos < data->size(); pos += kEntrySize) {
    Overlay ov;
    ov.id = le32(*data, pos);
    ov.ram_address = le32(*data, pos + 4);
    ov.ram_size = le32(*data, pos + 8);
    ov.bss_size = le32(*data, pos + 12);
    ov.static_init_start = le32(*data, pos + 16);
    ov.static_init_end = le32(*data, pos + 20);
    const std::uint32_t file_id = le32(*data, pos + 24);
    const std::uint32_t packed = le32(*data, pos + 28);
    if (file_id >= file_count) {
      return Error{ErrorCode::Corrupt, "overlay " + std::to_string(ov.id) +
                                           " refers to a file that does not exist"};
    }
    ov.file_id = static_cast<std::uint16_t>(file_id);
    ov.compressed_size = packed & 0x00FFFFFFu;
    ov.compressed = ((packed >> 24) & 1) != 0;
    overlays.push_back(ov);
  }
  return overlays;
}

}  // namespace bd::rom
