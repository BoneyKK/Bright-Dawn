#include <string>

#include "codecs.h"
#include "platform/paths.h"
#include "rom/export_path.h"
#include "rom/file_format.h"
#include "test.h"

using bd::rom::FileFormat;
using bd_test::Bytes;

namespace {

Bytes tagged(const char* tag, std::size_t size = 64) {
  Bytes data(tag, tag + 4);
  const Bytes body = bd_test::sample_data(size, 77);
  data.insert(data.end(), body.begin(), body.end());
  return data;
}

}  // namespace

TEST(sniff_recognises_documented_formats) {
  CHECK(bd::rom::sniff(tagged("SDAT")).format == FileFormat::Sdat);
  CHECK(bd::rom::sniff(tagged("NARC")).format == FileFormat::Narc);
  CHECK(bd::rom::sniff(tagged("BMD0")).format == FileFormat::Nsbmd);
  CHECK(bd::rom::sniff(tagged("BTX0")).format == FileFormat::Nsbtx);
  CHECK(bd::rom::sniff(tagged("BCA0")).format == FileFormat::Nsbca);
  CHECK(bd::rom::sniff(tagged("RLCN")).format == FileFormat::Nclr);
  CHECK(bd::rom::sniff(tagged("RGCN")).format == FileFormat::Ncgr);
  CHECK(bd::rom::sniff(tagged("RTFN")).format == FileFormat::Nftr);
  CHECK(bd::rom::sniff(Bytes{}).format == FileFormat::Empty);
  CHECK(bd::rom::sniff(Bytes{'S', 'D'}).format == FileFormat::Unknown);
  CHECK(bd::rom::sniff(tagged("WXYZ")).format == FileFormat::Unknown);
}

TEST(sniff_looks_inside_compressed_files) {
  const auto lz10 = bd::rom::sniff(bd_test::lz10_compress(tagged("BMD0", 500)));
  CHECK(lz10.format == FileFormat::Lz10);
  CHECK(lz10.inner == FileFormat::Nsbmd);
  const auto lz11 = bd::rom::sniff(bd_test::lz11_compress(tagged("BTX0", 500)));
  CHECK(lz11.format == FileFormat::Lz11);
  CHECK(lz11.inner == FileFormat::Nsbtx);
  const auto opaque = bd::rom::sniff(bd_test::lz10_compress(bd_test::sample_data(500, 2)));
  CHECK(opaque.format == FileFormat::Lz10);
  CHECK(opaque.inner == FileFormat::Unknown);
}

TEST(sniff_handles_the_0x40_scheme) {
  const Bytes packed = bd_test::lz40_compress(tagged("BCA0", 500));
  const auto good = bd::rom::sniff(packed);
  CHECK(good.format == FileFormat::Lz40);
  CHECK(good.inner == FileFormat::Nsbca);
  CHECK(bd::rom::is_compression(good.format));
  const auto unpacked = bd::rom::decompress(good.format, packed);
  REQUIRE(unpacked.ok());
  CHECK(unpacked.value() == tagged("BCA0", 500));

  // Up to three bytes of alignment padding are part of a normal file.
  Bytes padded = packed;
  padded.insert(padded.end(), 3, 0x00);
  CHECK(bd::rom::sniff(padded).format == FileFormat::Lz40);

  // A stream that stops short of the file, or does not decode, is reported
  // as unconfirmed rather than passed off as decoded.
  Bytes trailing = packed;
  trailing.insert(trailing.end(), 16, 0x55);
  CHECK(bd::rom::sniff(trailing).format == FileFormat::Lz40Unconfirmed);
  CHECK(!bd::rom::is_compression(FileFormat::Lz40Unconfirmed));
  const Bytes other_reading = bd_test::lz40_compress(tagged("BCA0", 500), bd_test::Lz40Flags::Plain);
  CHECK(bd::rom::sniff(other_reading).format == FileFormat::Lz40Unconfirmed);

  // An implausible header is just an unknown file that happens to start with '@'.
  CHECK(bd::rom::sniff(Bytes{0x40, 0, 0, 0, 0}).format == FileFormat::Unknown);
  Bytes text(400, 'a');
  text[0] = '@';
  text[1] = 4;
  text[2] = 0;
  text[3] = 0;
  CHECK(bd::rom::sniff(text).format == FileFormat::Unknown);
  CHECK(!bd::rom::decompress(FileFormat::Narc, packed).ok());
}

TEST(sniff_does_not_trust_a_lone_type_byte) {
  // Starts with 0x10 but is not a valid LZ10 stream.
  CHECK(bd::rom::sniff(Bytes{0x10, 0x40, 0x00, 0x00, 0xFF, 0xFF, 0xFF}).format ==
        FileFormat::Unknown);
}

TEST(every_format_has_a_name) {
  for (int i = 0; i <= static_cast<int>(bd::rom::kLastFileFormat); ++i) {
    const auto format = static_cast<FileFormat>(i);
    CHECK(std::string(bd::rom::name(format)).size() > 0);
    CHECK(std::string(bd::rom::description(format)).size() > 0);
  }
}

TEST(export_path_keeps_ordinary_paths) {
  const auto path = bd::rom::to_export_path("data/model/hero.nsbmd");
  REQUIRE(path.has_value());
  CHECK(path->generic_string() == "data/model/hero.nsbmd");
  CHECK(path->is_relative());
}

TEST(export_path_refuses_to_escape_the_destination) {
  CHECK(!bd::rom::to_export_path("").has_value());
  CHECK(!bd::rom::to_export_path("..").has_value());
  CHECK(!bd::rom::to_export_path("../evil").has_value());
  CHECK(!bd::rom::to_export_path("data/../../evil").has_value());
  CHECK(!bd::rom::to_export_path("/etc/passwd").has_value());
  CHECK(!bd::rom::to_export_path("data//file").has_value());
  CHECK(!bd::rom::to_export_path("data/./file").has_value());
  CHECK(!bd::rom::to_export_path("data/").has_value());
}

TEST(export_path_neutralises_reserved_characters) {
  const auto drive = bd::rom::to_export_path("C:\\Windows\\evil.dll");
  REQUIRE(drive.has_value());
  CHECK(drive->generic_string() == "C%3A%5CWindows%5Cevil.dll");
  CHECK(drive->is_relative());
  CHECK(!drive->has_root_name());

  const auto odd = bd::rom::to_export_path(std::string("a\x01" "b\x83\x41/trailing."));
  REQUIRE(odd.has_value());
  CHECK(odd->generic_string() == "a%01b%83A/trailing.%");
  // Distinct ROM names must stay distinct after escaping.
  CHECK(bd::rom::to_export_path("a%3A")->generic_string() !=
        bd::rom::to_export_path("a:")->generic_string());
}

TEST(user_data_dir_is_absolute) {
  const auto dir = bd::platform::user_data_dir();
  if (dir.ok()) {
    CHECK(dir.value().is_absolute());
    CHECK(!dir.value().filename().empty());
  } else {
    // Only acceptable when the environment gives us nothing to go on.
    CHECK(dir.error().code == bd::ErrorCode::Io);
  }
}
