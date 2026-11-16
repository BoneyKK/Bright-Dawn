#include <string_view>

#include <algorithm>
#include <optional>
#include <string>

#include "core/checksum.h"
#include "core/sha256.h"
#include "temp_dir.h"
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

TEST(sha256_matches_the_standard_test_vectors) {
  // FIPS 180-4's examples, and the empty message.
  const auto hash = [](const std::string& text) {
    bd::Sha256 h;
    h.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return bd::to_hex(h.finish());
  };
  CHECK(hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // A million "a"s, given in pieces that do not line up with the blocks.
  bd::Sha256 h;
  const std::string piece(997, 'a');
  std::size_t left = 1000000;
  while (left > 0) {
    const std::size_t n = std::min(left, piece.size());
    h.update(reinterpret_cast<const std::uint8_t*>(piece.data()), n);
    left -= n;
  }
  CHECK(bd::to_hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // A file, and one that is not there.
  bd_test::TempDir dir;
  dir.write("abc.txt", "abc");
  CHECK(bd::sha256_of_file(dir.path() / "abc.txt") ==
        std::optional<std::string>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK(!bd::sha256_of_file(dir.path() / "none.txt").has_value());
}
