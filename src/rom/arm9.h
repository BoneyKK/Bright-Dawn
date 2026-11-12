// Locating the SDK start-up parameters inside the ARM9 program and undoing
// the compression applied to it.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/nds_header.h"

namespace bd::rom {

// Block emitted by the SDK start-up code. It is identified by two marker
// words that follow it.
struct ModuleParams {
  std::uint32_t offset = 0;  // Position of the block inside the ARM9 image.
  std::uint32_t autoload_list_start = 0;
  std::uint32_t autoload_list_end = 0;
  std::uint32_t autoload_start = 0;
  std::uint32_t static_bss_start = 0;
  std::uint32_t static_bss_end = 0;
  std::uint32_t compressed_static_end = 0;  // 0 when the image is not compressed.
  std::uint32_t sdk_version = 0;

  unsigned sdk_major() const { return sdk_version >> 24; }
  unsigned sdk_minor() const { return (sdk_version >> 16) & 0xFF; }
  unsigned sdk_revision() const { return sdk_version & 0xFFFF; }
};

std::optional<ModuleParams> find_module_params(ByteSpan arm9);

// Returns the ARM9 program as it exists in memory after the start-up code has
// decompressed it. Images that are not compressed are returned unchanged.
Result<std::vector<std::uint8_t>> decompress_arm9(ByteSpan arm9, const CodeSection& section,
                                                  const std::optional<ModuleParams>& params);

}  // namespace bd::rom
