#include <string_view>

#include "core/checksum.h"
#include "test.h"

namespace {

bd::ByteSpan bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace

TEST(crc16_matches_published_check_value) {
  CHECK(bd::crc16(bytes_of("123456789")) == 0x4B37);
  CHECK(bd::crc16({}) == 0xFFFF);
}

TEST(crc32_matches_published_check_value) {
  CHECK(bd::crc32(bytes_of("123456789")) == 0xCBF43926u);
  CHECK(bd::crc32({}) == 0);
}

TEST(in_range_does_not_overflow) {
  CHECK(bd::in_range(10, 0, 10));
  CHECK(bd::in_range(10, 10, 0));
  CHECK(!bd::in_range(10, 11, 0));
  CHECK(!bd::in_range(10, 5, 6));
  CHECK(!bd::in_range(10, 0xFFFFFFFFFFFFFFFFull, 2));
  CHECK(!bd::in_range(10, 2, 0xFFFFFFFFFFFFFFFFull));
}
