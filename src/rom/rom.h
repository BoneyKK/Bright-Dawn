// A validated Nintendo DS ROM image held in memory.
//
// This is the only place that touches the user's ROM file. Everything else in
// the project receives data through this interface, so rendering, audio and
// game logic never depend on how or where the ROM is stored.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/arm9.h"
#include "rom/nds_header.h"
#include "rom/nitrofs.h"
#include "rom/overlay.h"

namespace bd::rom {

enum class SecureArea {
  Absent,     // The ARM9 program starts beyond the secure area.
  Decrypted,  // Normal for ROM dumps; the code is directly readable.
  Encrypted,  // The first 2 KiB of ARM9 code are still encrypted.
};

// How an overlay is stored in the ROM.
enum class OverlayEncoding {
  Raw,   // Stored exactly as it is loaded.
  Blz,   // Backward LZ, as flagged in the overlay table (the SDK's own scheme).
  Lz40,  // LZ40, not flagged in the table; this is what Dark Dawn uses.
};

struct OverlayImage {
  std::vector<std::uint8_t> data;  // Contents as they sit in memory.
  OverlayEncoding encoding = OverlayEncoding::Raw;
};

class Rom {
 public:
  // Reads and validates `path`. Structural problems are reported as NotNds,
  // Truncated or Corrupt. Any valid DS ROM is accepted here; use
  // require_dark_dawn() to check that it is the right game.
  static Result<Rom> open(const std::filesystem::path& path);
  static Result<Rom> from_bytes(std::vector<std::uint8_t> bytes);

  ByteSpan bytes() const { return bytes_; }
  const NdsHeader& header() const { return header_; }
  const NitroFs& fs() const { return fs_; }
  const std::vector<Overlay>& arm9_overlays() const { return arm9_overlays_; }
  const std::vector<Overlay>& arm7_overlays() const { return arm7_overlays_; }
  const std::optional<ModuleParams>& module_params() const { return module_params_; }
  SecureArea secure_area() const { return secure_area_; }

  // Conditions that do not prevent loading but that the user should know of.
  const std::vector<std::string>& warnings() const { return warnings_; }

  // Program images exactly as stored in the ROM.
  ByteSpan arm9() const;
  ByteSpan arm7() const;

  // The ARM9 program with its compression removed.
  Result<std::vector<std::uint8_t>> arm9_decompressed() const;
  // An overlay's contents with its compression removed. An overlay that the
  // table does not flag as compressed is still decoded when it is a complete
  // LZ40 file. Compare the result's size with overlay.ram_size to confirm
  // that it decoded to what the game expects.
  Result<OverlayImage> load_overlay(const Overlay& overlay) const;

  // Contents of a file by ID.
  Result<ByteSpan> file(std::uint16_t file_id) const;

 private:
  Rom() = default;

  std::vector<std::uint8_t> bytes_;
  NdsHeader header_;
  NitroFs fs_;
  std::vector<Overlay> arm9_overlays_;
  std::vector<Overlay> arm7_overlays_;
  std::optional<ModuleParams> module_params_;
  SecureArea secure_area_ = SecureArea::Absent;
  std::vector<std::string> warnings_;
};

}  // namespace bd::rom
