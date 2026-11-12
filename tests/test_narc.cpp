#include <string>

#include "codecs.h"
#include "rom/narc.h"
#include "test.h"

using bd::rom::Narc;
using bd_test::Bytes;

namespace {

Bytes span_bytes(bd::ByteSpan data) { return Bytes(data.begin(), data.end()); }

std::vector<bd_test::NarcMember> sample_members() {
  return {{"alpha.bin", bd_test::sample_data(100, 1)},
          {"beta.nsbmd", bd_test::sample_data(37, 2)},
          {"", Bytes{}},
          {"", bd_test::sample_data(9, 3)}};
}

}  // namespace

TEST(narc_reads_members_and_names) {
  const auto members = sample_members();
  const Bytes archive = bd_test::build_narc(members);
  const auto parsed = Narc::parse(archive);
  REQUIRE(parsed.ok());
  REQUIRE(parsed.value().file_count() == members.size());
  for (std::size_t i = 0; i < members.size(); ++i) {
    CHECK(span_bytes(parsed.value().file(i)) == members[i].data);
    CHECK(parsed.value().name(i) == members[i].name);
  }
}

TEST(narc_without_any_names) {
  const Bytes archive = bd_test::build_narc({{"", Bytes{1, 2, 3}}, {"", Bytes{4}}});
  const auto parsed = Narc::parse(archive);
  REQUIRE(parsed.ok());
  CHECK(parsed.value().file_count() == 2);
  CHECK(parsed.value().name(0).empty());
  CHECK(span_bytes(parsed.value().file(1)) == Bytes{4});

  const auto empty = Narc::parse(bd_test::build_narc({}));
  REQUIRE(empty.ok());
  CHECK(empty.value().file_count() == 0);
}

TEST(narc_accepts_name_table_pointing_into_its_own_header) {
  // Archives without names commonly store an 8-byte name table whose listing
  // offset is 4: the listing then reads the zero "first file" field and ends.
  Bytes archive = bd_test::build_narc({{"", Bytes{7, 7, 7}}});
  const std::string_view text(reinterpret_cast<const char*>(archive.data()), archive.size());
  const std::size_t btnf = text.find("BTNF");
  REQUIRE(btnf != std::string_view::npos);
  archive[btnf + 8] = 4;
  const auto parsed = Narc::parse(archive);
  REQUIRE(parsed.ok());
  CHECK(parsed.value().file_count() == 1);
}

TEST(narc_rejects_malformed_archives) {
  const Bytes good = bd_test::build_narc(sample_members());
  CHECK(!Narc::parse(Bytes{}).ok());
  CHECK(!Narc::parse(Bytes{'N', 'A', 'R', 'C'}).ok());
  Bytes wrong_magic = good;
  wrong_magic[0] = 'X';
  CHECK(!Narc::parse(wrong_magic).ok());
  for (std::size_t cut = 0; cut < good.size(); cut += 3)
    CHECK(!Narc::parse(bd::ByteSpan(good).first(cut)).ok());

  Bytes bad_member = good;  // First member's end beyond the data section.
  bad_member[16 + 8 + 4 + 4] = 0xFF;
  bad_member[16 + 8 + 4 + 5] = 0xFF;
  bad_member[16 + 8 + 4 + 6] = 0xFF;
  CHECK(!Narc::parse(bad_member).ok());

  Bytes bad_count = good;  // More members than the table holds.
  bad_count[16 + 8] = 0xFF;
  bad_count[16 + 8 + 1] = 0x7F;
  CHECK(!Narc::parse(bad_count).ok());
}

TEST(narc_survives_arbitrary_damage) {
  const Bytes good = bd_test::build_narc(sample_members());
  std::uint32_t state = 77;
  for (int i = 0; i < 5000; ++i) {
    Bytes damaged = good;
    for (int edits = 0; edits < 3; ++edits) {
      state = state * 1664525u + 1013904223u;
      damaged[(state >> 8) % damaged.size()] = static_cast<std::uint8_t>(state >> 24);
    }
    const auto parsed = Narc::parse(damaged);
    if (!parsed.ok()) continue;
    std::size_t total = 0;
    for (std::size_t m = 0; m < parsed.value().file_count(); ++m)
      total += parsed.value().file(m).size();
    CHECK(total <= damaged.size() * (parsed.value().file_count() + 1));
  }
}
