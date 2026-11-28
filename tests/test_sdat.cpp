// The sound archive (SDAT): its sequences, with names and events, from an
// archive made up here; and damaged archives, which are errors.
#include <cstdint>
#include <string>
#include <vector>

#include "rom/sdat.h"
#include "test.h"

namespace {

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put(std::vector<std::uint8_t>& b, std::size_t at, const std::string& s) {
  for (std::size_t i = 0; i < s.size(); ++i) b[at + i] = static_cast<std::uint8_t>(s[i]);
}

// An archive with three sequence numbers: 0 ("SEQ_ONE") and 2 ("SEQ_TWO")
// have sequences, 1 has none.
struct Archive {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x400, 0);
  static constexpr std::size_t kSymb = 0x40, kInfo = 0x100, kFat = 0x180, kFiles = 0x200;

  Archive() {
    put(bytes, 0, "SDAT");
    put16(bytes, 4, 0xFEFF);
    put16(bytes, 6, 0x0100);
    put32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    put16(bytes, 12, 0x40);
    put16(bytes, 14, 4);
    const std::uint32_t blocks[8] = {kSymb, 0xC0, kInfo, 0x80, kFat, 0x80, kFiles, 0x200};
    for (int i = 0; i < 8; ++i) put32(bytes, 0x10 + i * 4, blocks[i]);
    // SYMB: the sequences' record at 0x28, its names after it.
    put(bytes, kSymb, "SYMB");
    put32(bytes, kSymb + 8, 0x28);
    put32(bytes, kSymb + 0x28, 3);
    put32(bytes, kSymb + 0x2C, 0x40);
    put32(bytes, kSymb + 0x30, 0);
    put32(bytes, kSymb + 0x34, 0x48);
    put(bytes, kSymb + 0x40, "SEQ_ONE");
    put(bytes, kSymb + 0x48, "SEQ_TWO");
    // INFO: the record at 0x28, entries for 0 and 2.
    put(bytes, kInfo, "INFO");
    put32(bytes, kInfo + 8, 0x28);
    put32(bytes, kInfo + 0x28, 3);
    put32(bytes, kInfo + 0x2C, 0x40);
    put32(bytes, kInfo + 0x30, 0);
    put32(bytes, kInfo + 0x34, 0x50);
    entry(0x40, 0, 7, 100, 0);
    entry(0x50, 1, 9, 90, 2);
    // FAT: two files.
    put(bytes, kFat, "FAT ");
    put32(bytes, kFat + 8, 2);
    sseq(0, kFiles, {0x3C, 0x7F, 0x30, 0xFF});
    sseq(1, kFiles + 0x80, {0xFE, 0x01, 0x00, 0xFF, 0x55});
  }
  void entry(std::size_t at, unsigned file, unsigned bank, unsigned volume, unsigned player) {
    put16(bytes, kInfo + at, file);
    put16(bytes, kInfo + at + 4, bank);
    bytes[kInfo + at + 6] = static_cast<std::uint8_t>(volume);
    bytes[kInfo + at + 9] = static_cast<std::uint8_t>(player);
  }
  void sseq(unsigned n, std::size_t at, const std::vector<std::uint8_t>& events) {
    const auto size = static_cast<std::uint32_t>(0x1C + events.size());
    put32(bytes, kFat + 12 + n * 16, static_cast<std::uint32_t>(at));
    put32(bytes, kFat + 16 + n * 16, size);
    put(bytes, at, "SSEQ");
    put16(bytes, at + 4, 0xFEFF);
    put32(bytes, at + 8, size);
    put16(bytes, at + 12, 0x10);
    put16(bytes, at + 14, 1);
    put(bytes, at + 0x10, "DATA");
    put32(bytes, at + 0x14, size - 0x10);
    put32(bytes, at + 0x18, 0x1C);
    for (std::size_t i = 0; i < events.size(); ++i) bytes[at + 0x1C + i] = events[i];
  }
};

}  // namespace

TEST(sdat_lists_its_sequences_with_their_names_and_events) {
  Archive a;
  auto list = bd::rom::sdat_sequences(a.bytes);
  REQUIRE(list.ok());
  REQUIRE(list.value().size() == 2);
  const bd::rom::SdatSequence& one = list.value()[0];
  const bd::rom::SdatSequence& two = list.value()[1];
  CHECK(one.index == 0 && one.name == "SEQ_ONE" && one.bank == 7 && one.volume == 100 && one.player == 0);
  CHECK(one.data.size() == 4 && one.data[0] == 0x3C && one.data[3] == 0xFF);
  CHECK(two.index == 2 && two.name == "SEQ_TWO" && two.bank == 9 && two.volume == 90 && two.player == 2);
  CHECK(two.data.size() == 5 && two.data[0] == 0xFE && two.data[4] == 0x55);
  // Without names, the sequences are still there.
  a.bytes[Archive::kSymb] = 'X';
  list = bd::rom::sdat_sequences(a.bytes);
  REQUIRE(list.ok() && list.value().size() == 2);
  CHECK(list.value()[0].name.empty() && list.value()[1].data.size() == 5);
}

TEST(sdat_reports_damage_without_reading_past_it) {
  CHECK(!bd::rom::sdat_sequences({}).ok());
  {
    Archive a;
    a.bytes.resize(0x30);  // The header, cut short.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put32(a.bytes, 0x18, 0x10000);  // INFO past the end.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put32(a.bytes, Archive::kInfo + 0x28, 0x7FFFFFFF);  // More sequences than there is room for.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put16(a.bytes, Archive::kInfo + 0x40, 5);  // A file past the table.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put32(a.bytes, Archive::kFat + 16, 0x10000);  // A file past the archive.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put32(a.bytes, Archive::kFiles + 0x18, 0x1000);  // Events past the file.
    CHECK(!bd::rom::sdat_sequences(a.bytes).ok());
  }
  {
    Archive a;
    put32(a.bytes, Archive::kSymb + 0x2C, 0xFFFFFF);  // A name past the block: no name.
    auto list = bd::rom::sdat_sequences(a.bytes);
    REQUIRE(list.ok());
    CHECK(list.value()[0].name.empty() && list.value()[1].name == "SEQ_TWO");
  }
}
