// The cartridge's save memory chip.
#include <cstdint>
#include <vector>

#include "nds/save_memory.h"
#include "test.h"

using bd::nds::SaveMemory;

TEST(save_memory_starts_erased) {
  SaveMemory save;
  CHECK(save.contents().size() == 262144);
  bool erased = true;
  for (const std::uint8_t byte : save.contents()) erased = erased && byte == 0xFF;
  CHECK(erased);
  CHECK(!save.changed());
  CHECK(save.status() == 0);
  CHECK(save.read(0) == 0xFF && save.read(262143) == 0xFF);
  CHECK(save.statistics().bytes_read == 2);
}

TEST(save_memory_page_write_replaces_bytes) {
  SaveMemory save;
  const std::uint8_t first[] = {0x12, 0x34, 0x56};
  save.page_write(0x1000, first, 3);
  CHECK(save.read(0x1000) == 0x12 && save.read(0x1001) == 0x34 && save.read(0x1002) == 0x56);
  CHECK(save.read(0x0FFF) == 0xFF && save.read(0x1003) == 0xFF);
  CHECK(save.changed());
  const std::uint8_t second[] = {0xFF, 0x00};
  save.page_write(0x1001, second, 2);  // Bits can go back to 1 this way.
  CHECK(save.read(0x1000) == 0x12 && save.read(0x1001) == 0xFF && save.read(0x1002) == 0x00);
  CHECK(save.statistics().bytes_written == 5);
}

TEST(save_memory_page_program_only_clears_bits) {
  SaveMemory save;
  const std::uint8_t first[] = {0xF0, 0x3C};
  save.page_program(0x2000, first, 2);
  CHECK(save.read(0x2000) == 0xF0 && save.read(0x2001) == 0x3C);
  const std::uint8_t second[] = {0x0F, 0xFF};
  save.page_program(0x2000, second, 2);
  CHECK(save.read(0x2000) == 0x00);  // 0xF0 and 0x0F.
  CHECK(save.read(0x2001) == 0x3C);  // Programming all ones changes nothing.
}

TEST(save_memory_writes_stay_inside_their_page) {
  SaveMemory save;
  const std::uint8_t data[] = {1, 2, 3, 4};
  save.page_write(0x30FE, data, 4);  // Two bytes before the page's end.
  CHECK(save.read(0x30FE) == 1 && save.read(0x30FF) == 2);
  CHECK(save.read(0x3000) == 3 && save.read(0x3001) == 4);  // Continued at the page's start,
  CHECK(save.read(0x3100) == 0xFF);                         // not in the next page.
  const std::uint8_t zeros[] = {0, 0, 0};
  save.page_program(0x41FF, zeros, 3);
  CHECK(save.read(0x41FF) == 0 && save.read(0x4100) == 0 && save.read(0x4101) == 0 && save.read(0x4200) == 0xFF);
}

TEST(save_memory_erases_whole_sectors) {
  SaveMemory save;
  const std::uint8_t data[] = {0};
  save.page_write(0x0FFFF, data, 1);  // Last byte of the first sector,
  save.page_write(0x10000, data, 1);  // first and
  save.page_write(0x1FFFF, data, 1);  // last of the second,
  save.page_write(0x20000, data, 1);  // first of the third.
  save.erase_sector(0x12345);         // Anywhere inside the second.
  CHECK(save.read(0x0FFFF) == 0 && save.read(0x10000) == 0xFF && save.read(0x1FFFF) == 0xFF && save.read(0x20000) == 0);
  CHECK(save.statistics().sectors_erased == 1);
}

TEST(save_memory_addresses_wrap_at_its_size) {
  SaveMemory save;
  const std::uint8_t data[] = {0xAB};
  save.page_write(0x40000 + 0x55, data, 1);
  CHECK(save.read(0x55) == 0xAB && save.read(0x80055) == 0xAB);
  save.erase_sector(0x40000);
  CHECK(save.read(0x55) == 0xFF);
}

TEST(save_memory_loads_and_reports_changes) {
  SaveMemory save;
  std::vector<std::uint8_t> file(262144);
  for (std::size_t i = 0; i < file.size(); ++i) file[i] = static_cast<std::uint8_t>(i * 7);
  CHECK(save.load(bd::ByteSpan(file.data(), file.size())).ok());
  CHECK(!save.changed());
  CHECK(save.read(3) == 21 && save.read(262143) == static_cast<std::uint8_t>(262143 * 7));
  const std::uint8_t same[] = {21};
  save.page_write(3, same, 1);  // Writing what is there is not a change.
  CHECK(!save.changed());
  save.page_program(3, same, 1);
  CHECK(!save.changed());
  const std::uint8_t other[] = {22};
  save.page_write(3, other, 1);
  CHECK(save.changed());
  CHECK(save.contents()[3] == 22);

  // A file of the wrong size is refused and changes nothing.
  SaveMemory second;
  CHECK(!second.load(bd::ByteSpan(file.data(), file.size() - 1)).ok());
  CHECK(!second.load(bd::ByteSpan()).ok());
  CHECK(second.read(3) == 0xFF);
  // Erasing an erased sector is not a change either.
  second.erase_sector(0);
  CHECK(!second.changed());
}
