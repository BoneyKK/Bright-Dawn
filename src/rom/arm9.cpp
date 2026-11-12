#include "rom/arm9.h"

#include "rom/compression.h"

namespace bd::rom {
namespace {

// The block ends with a marker word followed by the same word byte-swapped.
// Retail images store the bytes 21 06 C0 DE first. The opposite order is
// accepted too, because nothing else in a program looks like either pair.
constexpr std::uint32_t kMarker = 0xDEC00621;
constexpr std::uint32_t kMarkerSwapped = 0x2106C0DE;
constexpr std::size_t kParamsSize = 28;  // Seven words precede the markers.

bool is_marker_pair(std::uint32_t first, std::uint32_t second) {
  return (first == kMarker && second == kMarkerSwapped) ||
         (first == kMarkerSwapped && second == kMarker);
}

}  // namespace

std::optional<ModuleParams> find_module_params(ByteSpan arm9) {
  if (arm9.size() < kParamsSize + 8) return std::nullopt;
  for (std::size_t pos = kParamsSize; pos + 8 <= arm9.size(); pos += 4) {
    if (!is_marker_pair(le32(arm9, pos), le32(arm9, pos + 4))) continue;
    const std::size_t base = pos - kParamsSize;
    ModuleParams p;
    p.offset = static_cast<std::uint32_t>(base);
    p.autoload_list_start = le32(arm9, base);
    p.autoload_list_end = le32(arm9, base + 4);
    p.autoload_start = le32(arm9, base + 8);
    p.static_bss_start = le32(arm9, base + 12);
    p.static_bss_end = le32(arm9, base + 16);
    p.compressed_static_end = le32(arm9, base + 20);
    p.sdk_version = le32(arm9, base + 24);
    return p;
  }
  return std::nullopt;
}

Result<std::vector<std::uint8_t>> decompress_arm9(ByteSpan arm9, const CodeSection& section,
                                                  const std::optional<ModuleParams>& params) {
  if (!params || params->compressed_static_end == 0)
    return std::vector<std::uint8_t>(arm9.begin(), arm9.end());

  // The field holds the memory address one past the compressed data.
  const std::uint32_t end_address = params->compressed_static_end;
  if (end_address < section.ram_address || end_address - section.ram_address > arm9.size()) {
    return Error{ErrorCode::Corrupt,
                 "ARM9 compression marker points outside the ARM9 program"};
  }
  const std::size_t end = end_address - section.ram_address;
  auto decoded = blz_decompress(arm9.first(end));
  if (!decoded) {
    return Error{decoded.error().code, "cannot decompress the ARM9 program: " +
                                           decoded.error().message};
  }
  return decoded;
}

}  // namespace bd::rom
