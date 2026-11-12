#include <algorithm>
#include <string>

#include "rom/game_id.h"
#include "rom/rom.h"
#include "rom_builder.h"
#include "test.h"

using bd::ErrorCode;
using bd::rom::Rom;
using bd_test::Bytes;

namespace {

Bytes contents(const Rom& rom, const char* path) {
  const auto* file = rom.fs().find(path);
  if (file == nullptr) return {0xDE, 0xAD};
  const bd::ByteSpan data = rom.file(file->file_id).value();
  return Bytes(data.begin(), data.end());
}

ErrorCode open_error(Bytes image) {
  const auto rom = Rom::from_bytes(std::move(image));
  return rom.ok() ? static_cast<ErrorCode>(-1) : rom.error().code;
}

}  // namespace

TEST(rom_parses_header_fields) {
  const auto built = bd_test::build_rom(bd_test::default_spec());
  const auto opened = Rom::from_bytes(built.image);
  REQUIRE(opened.ok());
  const auto& h = opened.value().header();
  CHECK(h.title == "SYNTHETIC");
  CHECK(h.game_code == "BO5E");
  CHECK(h.maker_code == "01");
  CHECK(h.rom_version == 0);
  CHECK(h.arm9.rom_offset == 0x4000);
  CHECK(h.arm9.ram_address == 0x02000000);
  CHECK(h.used_rom_size == built.image.size());
  CHECK(h.header_size == 0x4000);
  CHECK(h.chip_size() == 128 * 1024);
}

TEST(rom_lists_and_reads_files) {
  const auto spec = bd_test::default_spec();
  const auto opened = Rom::from_bytes(bd_test::build_rom(spec).image);
  REQUIRE(opened.ok());
  const Rom& rom = opened.value();

  CHECK(rom.fs().named_files().size() == spec.files.size());
  CHECK(rom.fs().ranges().size() == spec.files.size() + spec.overlays.size());
  CHECK(rom.fs().directory_count() == 6);  // root, data, data/model, data/font, script, script/ev
  for (const auto& [path, data] : spec.files) {
    CHECK(rom.fs().find(path) != nullptr);
    CHECK(contents(rom, path.c_str()) == data);
  }
  CHECK(rom.fs().find("data/model") == nullptr);  // A directory is not a file.
  CHECK(rom.fs().find("missing.bin") == nullptr);
  CHECK(!rom.file(60000).ok());

  // Listing is ordered by file ID and IDs map back to their paths.
  const auto& named = rom.fs().named_files();
  CHECK(std::is_sorted(named.begin(), named.end(),
                       [](const auto& a, const auto& b) { return a.file_id < b.file_id; }));
  for (const auto& file : named) {
    REQUIRE(rom.fs().path_of(file.file_id) != nullptr);
    CHECK(*rom.fs().path_of(file.file_id) == file.path);
  }
  CHECK(rom.fs().path_of(0) == nullptr);  // Overlay files have no name.
}

TEST(rom_decompresses_arm9_and_overlays) {
  const auto spec = bd_test::default_spec();
  const auto built = bd_test::build_rom(spec);
  const auto opened = Rom::from_bytes(built.image);
  REQUIRE(opened.ok());
  const Rom& rom = opened.value();

  REQUIRE(rom.module_params().has_value());
  CHECK(rom.module_params()->offset == 0x40);
  CHECK(rom.module_params()->sdk_major() == 4);
  CHECK(rom.module_params()->sdk_minor() == 2);
  CHECK(rom.module_params()->compressed_static_end != 0);
  CHECK(rom.secure_area() == bd::rom::SecureArea::Decrypted);

  CHECK(rom.arm9().size() < built.arm9_plain.size());
  const auto arm9 = rom.arm9_decompressed();
  REQUIRE(arm9.ok());
  CHECK(arm9.value() == built.arm9_plain);

  REQUIRE(rom.arm9_overlays().size() == spec.overlays.size());
  for (std::size_t i = 0; i < spec.overlays.size(); ++i) {
    const auto& overlay = rom.arm9_overlays()[i];
    CHECK(overlay.id == i);
    CHECK(overlay.compressed == spec.overlays[i].compressed);
    CHECK(overlay.ram_address == spec.overlays[i].ram_address);
    CHECK(overlay.ram_size == spec.overlays[i].data.size());
    const auto data = rom.overlay_data(overlay);
    REQUIRE(data.ok());
    CHECK(data.value() == spec.overlays[i].data);
  }
  CHECK(rom.arm7_overlays().empty());
}

TEST(rom_handles_uncompressed_arm9) {
  auto spec = bd_test::default_spec();
  spec.compress_arm9 = false;
  const auto built = bd_test::build_rom(spec);
  const auto opened = Rom::from_bytes(built.image);
  REQUIRE(opened.ok());
  CHECK(opened.value().module_params()->compressed_static_end == 0);
  const auto arm9 = opened.value().arm9_decompressed();
  REQUIRE(arm9.ok());
  CHECK(arm9.value() == built.arm9_plain);
}

TEST(rom_reports_unusual_images_as_warnings) {
  auto spec = bd_test::default_spec();
  const auto normal = Rom::from_bytes(bd_test::build_rom(spec).image);
  REQUIRE(normal.ok());
  // The generated ROM carries a stand-in logo, which is worth one warning.
  CHECK(normal.value().warnings().size() == 1);

  spec.decrypted_secure_area = false;
  spec.with_module_params = false;
  const auto odd = Rom::from_bytes(bd_test::build_rom(spec).image);
  REQUIRE(odd.ok());
  CHECK(odd.value().secure_area() == bd::rom::SecureArea::Encrypted);
  CHECK(!odd.value().module_params().has_value());
  CHECK(odd.value().warnings().size() == 3);
  CHECK(odd.value().arm9_decompressed().ok());
}

TEST(rom_handles_no_files_at_all) {
  bd_test::RomSpec spec;
  const auto opened = Rom::from_bytes(bd_test::build_rom(spec).image);
  REQUIRE(opened.ok());
  CHECK(opened.value().fs().named_files().empty());
  CHECK(opened.value().fs().directory_count() == 1);
  CHECK(opened.value().arm9_overlays().empty());
}

TEST(game_identification) {
  auto spec = bd_test::default_spec();
  for (const char* code : {"BO5E", "BO5P", "BO5J"}) {
    spec.game_code = code;
    const auto opened = Rom::from_bytes(bd_test::build_rom(spec).image);
    REQUIRE(opened.ok());
    const auto* title = bd::rom::identify_title(opened.value().header());
    REQUIRE(title != nullptr);
    CHECK(title->game_code == code);
    CHECK(title->region == (code[3] == 'E'   ? bd::rom::Region::Usa
                            : code[3] == 'P' ? bd::rom::Region::Europe
                                             : bd::rom::Region::Japan));
    CHECK(bd::rom::require_dark_dawn(opened.value().header()).ok());
  }

  spec.game_code = "AGS\x01";
  spec.title = "SOMETHING";
  const auto other = Rom::from_bytes(bd_test::build_rom(spec).image);
  REQUIRE(other.ok());  // Structurally fine: it is simply a different game.
  CHECK(bd::rom::identify_title(other.value().header()) == nullptr);
  const auto status = bd::rom::require_dark_dawn(other.value().header());
  REQUIRE(!status.ok());
  CHECK(status.error().code == ErrorCode::WrongGame);
  CHECK(status.error().message.find("SOMETHING") != std::string::npos);
  CHECK(status.error().message.find("AGS?") != std::string::npos);  // Control byte not echoed.
  CHECK(status.error().message.find("BO5E") != std::string::npos);
}

TEST(rom_rejects_things_that_are_not_roms) {
  CHECK(open_error({}) == ErrorCode::NotNds);
  CHECK(open_error(Bytes(100, 0)) == ErrorCode::NotNds);
  CHECK(open_error(Bytes(0x10000, 0)) == ErrorCode::NotNds);
  CHECK(open_error(bd_test::sample_data(0x10000, 99)) == ErrorCode::NotNds);

  Bytes zip = bd_test::sample_data(0x1000, 5);
  zip[0] = 'P'; zip[1] = 'K'; zip[2] = 3; zip[3] = 4;
  const auto opened = Rom::from_bytes(zip);
  REQUIRE(!opened.ok());
  CHECK(opened.error().code == ErrorCode::NotNds);
  CHECK(opened.error().message.find("ZIP") != std::string::npos);
}

TEST(rom_detects_damaged_header) {
  auto image = bd_test::build_rom(bd_test::default_spec()).image;
  image[0x24] ^= 0x01;  // Flip one bit without fixing the checksum.
  CHECK(open_error(image) == ErrorCode::NotNds);
}

TEST(rom_detects_truncation) {
  const auto image = bd_test::build_rom(bd_test::default_spec()).image;
  for (std::size_t keep : {image.size() - 1, image.size() / 2, std::size_t{0x4000}, std::size_t{0x200}}) {
    Bytes cut(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(keep));
    CHECK(open_error(cut) == ErrorCode::Truncated);
  }
  // Padding after the used area (an untrimmed dump) is fine.
  Bytes padded = image;
  padded.resize(image.size() + 0x1000, 0xFF);
  CHECK(Rom::from_bytes(padded).ok());
}

TEST(rom_detects_out_of_range_sections) {
  const auto built = bd_test::build_rom(bd_test::default_spec());
  const auto size = static_cast<std::uint32_t>(built.image.size());

  Bytes image = built.image;  // ARM9 beyond the end of the file.
  bd_test::put_le32(image, 0x2C, size);
  bd_test::fix_header_crc(image);
  CHECK(open_error(image) == ErrorCode::Truncated);

  image = built.image;  // ARM7 offset that would overflow 32-bit arithmetic.
  bd_test::put_le32(image, 0x30, 0xFFFFFFF0);
  bd_test::fix_header_crc(image);
  CHECK(open_error(image) == ErrorCode::Truncated);

  image = built.image;  // Name table outside the image.
  bd_test::put_le32(image, 0x40, size - 4);
  bd_test::fix_header_crc(image);
  CHECK(open_error(image) == ErrorCode::Truncated);

  image = built.image;  // Allocation table with a ragged size.
  bd_test::put_le32(image, 0x4C, built.fat_size - 3);
  bd_test::fix_header_crc(image);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // Overlay table with a ragged size.
  bd_test::put_le32(image, 0x54, 33);
  bd_test::fix_header_crc(image);
  CHECK(open_error(image) == ErrorCode::Corrupt);
}

TEST(rom_detects_corrupt_file_tables) {
  const auto built = bd_test::build_rom(bd_test::default_spec());

  Bytes image = built.image;  // File that ends before it starts.
  bd_test::put_le32(image, built.fat_offset + 8 * 4, 0x9000);
  bd_test::put_le32(image, built.fat_offset + 8 * 4 + 4, 0x8000);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // File that extends past the image.
  bd_test::put_le32(image, built.fat_offset + 8 * 4 + 4, 0x7FFFFFFF);
  CHECK(open_error(image) == ErrorCode::Truncated);

  image = built.image;  // Directory count of zero.
  bd_test::put_le16(image, built.fnt_offset + 6, 0);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // Directory count larger than the table.
  bd_test::put_le16(image, built.fnt_offset + 6, 0x0FFF);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // Root listing that starts outside the table.
  bd_test::put_le32(image, built.fnt_offset, built.fnt_size + 100);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // First file ID beyond the allocation table.
  bd_test::put_le16(image, built.fnt_offset + 4, 0x4000);
  CHECK(open_error(image) == ErrorCode::Corrupt);

  image = built.image;  // Overlay pointing at a file that does not exist.
  bd_test::put_le32(image, built.overlay_table_offset + 24, 500);
  CHECK(open_error(image) == ErrorCode::Corrupt);
}

TEST(rom_rejects_directory_cycles) {
  const auto built = bd_test::build_rom(bd_test::default_spec());
  Bytes image = built.image;
  // Make every directory's listing start where the root's does. The root then
  // contains a subdirectory whose listing is the root's own: a cycle.
  const std::uint32_t root_listing = bd::le32(image, built.fnt_offset);
  const std::uint32_t dir_count = bd::le16(image, built.fnt_offset + 6);
  for (std::uint32_t d = 1; d < dir_count; ++d)
    bd_test::put_le32(image, built.fnt_offset + d * 8, root_listing);
  CHECK(open_error(image) == ErrorCode::Corrupt);
}

TEST(rom_reports_damaged_compressed_code) {
  auto spec = bd_test::default_spec();
  const auto built = bd_test::build_rom(spec);

  Bytes image = built.image;  // Compression marker pointing far outside the program.
  bd_test::put_le32(image, 0x4000 + 0x40 + 20, 0x02F00000);
  auto opened = Rom::from_bytes(image);
  REQUIRE(opened.ok());
  auto arm9 = opened.value().arm9_decompressed();
  REQUIRE(!arm9.ok());
  CHECK(arm9.error().code == ErrorCode::Corrupt);

  image = built.image;  // Zero the trailer of the first (compressed) overlay.
  const std::uint32_t end = bd::le32(image, built.fat_offset + 4);
  for (std::uint32_t i = end - 8; i < end; ++i) image[i] = 0;
  opened = Rom::from_bytes(image);
  REQUIRE(opened.ok());
  const auto overlay = opened.value().overlay_data(opened.value().arm9_overlays()[0]);
  REQUIRE(!overlay.ok());
  CHECK(overlay.error().message.find("overlay 0") != std::string::npos);
}

// Randomly damages the structural parts of a valid image. The parser may
// accept or reject each variant, but must never crash, hang or read out of
// bounds; run under the sanitizer build to make the last of those visible.
TEST(rom_survives_random_structural_damage) {
  const auto built = bd_test::build_rom(bd_test::default_spec());
  struct Region {
    std::uint32_t offset, size;
  };
  const Region regions[] = {
      {0x000, 0x160},
      {0x4000, 0x100},
      {built.fnt_offset, built.fnt_size},
      {built.fat_offset, built.fat_size},
      {built.overlay_table_offset, 96},
  };
  std::uint32_t state = 2024;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  int accepted = 0;
  for (int i = 0; i < 6000; ++i) {
    Bytes image = built.image;
    const int edits = 1 + static_cast<int>(next() % 4);
    for (int e = 0; e < edits; ++e) {
      const Region& region = regions[next() % 5];
      image[region.offset + next() % region.size] = static_cast<std::uint8_t>(next());
    }
    bd_test::fix_header_crc(image);
    const auto opened = Rom::from_bytes(std::move(image));
    if (!opened.ok()) continue;
    ++accepted;
    const Rom& rom = opened.value();
    (void)rom.arm9_decompressed();
    for (const auto& overlay : rom.arm9_overlays()) (void)rom.overlay_data(overlay);
    std::size_t total = 0;
    for (const auto& file : rom.fs().named_files()) total += rom.file(file.file_id).value().size();
    CHECK(total <= rom.bytes().size() * rom.fs().named_files().size() + 1);
  }
  // Sanity check on the test itself: both outcomes should occur.
  CHECK(accepted > 100);
  CHECK(accepted < 5900);
}
