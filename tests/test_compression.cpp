#include <utility>

#include "codecs.h"
#include "rom/compression.h"
#include "test.h"

using bd_test::Bytes;

namespace {

// Inputs chosen to hit the interesting encoder paths: empty, tiny, long runs
// (overlapping copies, long LZ11 tokens) and mixed data.
std::vector<Bytes> samples() {
  std::vector<Bytes> out;
  out.push_back({});
  out.push_back({0x42});
  out.push_back({1, 2, 3, 4, 5, 6, 7});
  out.push_back(Bytes(40, 0xAA));
  out.push_back(Bytes(300, 0x00));    // Needs the 3-byte LZ11 token.
  out.push_back(Bytes(70000, 0x5C));  // Needs the 4-byte LZ11 token.
  out.push_back(bd_test::sample_data(5000, 1));
  out.push_back(bd_test::sample_data(12345, 2));
  return out;
}

}  // namespace

TEST(lz10_round_trip) {
  for (const Bytes& raw : samples()) {
    const auto decoded = bd::rom::lz10_decompress(bd_test::lz10_compress(raw));
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == raw);
  }
}

TEST(lz10_known_stream) {
  // "abcabcabcabc": three literals, then one back-reference (length 9, distance 3).
  const Bytes stream = {0x10, 12, 0, 0, 0x10, 'a', 'b', 'c', 0x60, 0x02};
  const auto decoded = bd::rom::lz10_decompress(stream);
  REQUIRE(decoded.ok());
  CHECK(decoded.value() == Bytes({'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'}));
}

TEST(lz10_rejects_bad_streams) {
  CHECK(!bd::rom::lz10_decompress(Bytes{}).ok());
  CHECK(!bd::rom::lz10_decompress(Bytes{0x11, 4, 0, 0, 0, 1, 2, 3, 4}).ok());  // wrong type
  CHECK(!bd::rom::lz10_decompress(Bytes{0x10, 8, 0, 0, 0x00, 1, 2}).ok());     // ends early
  // Back-reference before the start of the output.
  CHECK(!bd::rom::lz10_decompress(Bytes{0x10, 8, 0, 0, 0x80, 0x00, 0x05}).ok());
  // Back-reference that would overrun the declared size.
  CHECK(!bd::rom::lz10_decompress(Bytes{0x10, 4, 0, 0, 0x40, 'x', 0xF0, 0x00}).ok());
  // Declared size beyond the sanity limit (extended 32-bit size field).
  const auto huge = bd::rom::lz10_decompress(Bytes{0x10, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0});
  REQUIRE(!huge.ok());
  CHECK(huge.error().code == bd::ErrorCode::Unsupported);
}

TEST(lz10_truncation_never_succeeds_silently) {
  const Bytes raw = bd_test::sample_data(2000, 4);
  const Bytes packed = bd_test::lz10_compress(raw);
  for (std::size_t cut = 0; cut < packed.size(); cut += 7) {
    const auto decoded = bd::rom::lz10_decompress(bd::ByteSpan(packed).first(cut));
    CHECK(!decoded.ok());
  }
}

TEST(lz11_round_trip) {
  for (const Bytes& raw : samples()) {
    const auto decoded = bd::rom::lz11_decompress(bd_test::lz11_compress(raw));
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == raw);
  }
}

TEST(lz11_rejects_bad_streams) {
  CHECK(!bd::rom::lz11_decompress(Bytes{0x11}).ok());
  CHECK(!bd::rom::lz11_decompress(Bytes{0x10, 4, 0, 0, 0, 1, 2, 3, 4}).ok());
  CHECK(!bd::rom::lz11_decompress(Bytes{0x11, 8, 0, 0, 0x80, 0x20}).ok());        // token cut short
  CHECK(!bd::rom::lz11_decompress(Bytes{0x11, 8, 0, 0, 0x80, 0x20, 0x09}).ok());  // bad distance
  const Bytes packed = bd_test::lz11_compress(Bytes(70000, 0x5C));
  for (std::size_t cut = 0; cut < packed.size(); ++cut)
    CHECK(!bd::rom::lz11_decompress(bd::ByteSpan(packed).first(cut)).ok());
}

TEST(blz_round_trip) {
  for (std::size_t prefix : {std::size_t{0}, std::size_t{1}, std::size_t{0x100}}) {
    for (std::uint32_t seed = 1; seed <= 4; ++seed) {
      const Bytes raw = bd_test::sample_data(3000 + seed * 517, seed);
      const Bytes packed = bd_test::blz_compress(raw, prefix);
      CHECK(packed.size() < raw.size());
      CHECK(packed.size() % 4 == 0);
      const auto decoded = bd::rom::blz_decompress(packed);
      REQUIRE(decoded.ok());
      CHECK(decoded.value() == raw);
    }
  }
}

TEST(blz_rejects_bad_trailers) {
  CHECK(!bd::rom::blz_decompress(Bytes{1, 2, 3}).ok());
  // Trailer shorter than its own fixed part.
  CHECK(!bd::rom::blz_decompress(Bytes{0, 0, 0, 0, 8, 0, 0, 4, 1, 0, 0, 0}).ok());
  // Compressed region longer than the input.
  CHECK(!bd::rom::blz_decompress(Bytes{0xFF, 0xFF, 0x00, 8, 1, 0, 0, 0}).ok());
  // Implausible decompressed size.
  const auto huge = bd::rom::blz_decompress(Bytes{8, 0, 0, 8, 0xFF, 0xFF, 0xFF, 0xFF});
  REQUIRE(!huge.ok());
  CHECK(huge.error().code == bd::ErrorCode::Unsupported);
}

TEST(blz_survives_arbitrary_damage) {
  const Bytes raw = bd_test::sample_data(4000, 6);
  const Bytes packed = bd_test::blz_compress(raw, 16);
  std::uint32_t state = 12345;
  for (int i = 0; i < 4000; ++i) {
    Bytes damaged = packed;
    state = state * 1664525u + 1013904223u;
    damaged[(state >> 8) % damaged.size()] ^= static_cast<std::uint8_t>(1u << (state & 7));
    // Any outcome is acceptable except a crash or an out-of-bounds access,
    // which the sanitizer build turns into a test failure.
    (void)bd::rom::blz_decompress(damaged);
  }
}

TEST(lz40_round_trip_in_every_variant) {
  using bd::rom::Lz40Options;
  const std::pair<bd_test::Lz40Flags, Lz40Options::Flags> modes[] = {
      {bd_test::Lz40Flags::Negated, Lz40Options::Flags::Negated},
      {bd_test::Lz40Flags::Plain, Lz40Options::Flags::Plain},
      {bd_test::Lz40Flags::Inverted, Lz40Options::Flags::Inverted},
  };
  for (const auto& [encoder_flags, decoder_flags] : modes) {
    for (bool big_endian : {true, false}) {
      for (const Bytes& raw : samples()) {
        const Bytes packed = bd_test::lz40_compress(raw, encoder_flags, big_endian);
        std::size_t consumed = 0;
        const auto decoded =
            bd::rom::lz40_decompress(packed, &consumed, Lz40Options{decoder_flags, big_endian});
        REQUIRE(decoded.ok());
        CHECK(decoded.value() == raw);
        CHECK(consumed == packed.size());
      }
    }
  }
}

TEST(lz40_known_stream) {
  // Logical flags 0b00010000 (three literals, then a back-reference) are stored
  // negated as 0xF0. The token 0x0039 means distance 3, length 9.
  const Bytes stream = {0x40, 12, 0, 0, 0xF0, 'a', 'b', 'c', 0x39, 0x00};
  const auto decoded = bd::rom::lz40_decompress(stream);
  REQUIRE(decoded.ok());
  CHECK(decoded.value() == Bytes({'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'}));

  // Length code 0: one extra byte holding length - 16. Logical flags
  // 0b01000000 (a literal, then a back-reference) are stored as 0xC0. The
  // token 0x0010 means distance 1; the extra byte gives length 20.
  const Bytes medium = {0x40, 21, 0, 0, 0xC0, 'x', 0x10, 0x00, 0x04};
  const auto run = bd::rom::lz40_decompress(medium);
  REQUIRE(run.ok());
  CHECK(run.value() == Bytes(21, 'x'));
}

TEST(lz40_variants_disagree_on_real_streams) {
  // The probe relies on a wrong reading failing rather than quietly producing
  // different output. Long matches distinguish the two byte orders.
  using bd::rom::Lz40Options;
  const Bytes raw = bd_test::sample_data(6000, 12);
  const Bytes packed = bd_test::lz40_compress(raw);
  for (auto flags : {Lz40Options::Flags::Plain, Lz40Options::Flags::Inverted}) {
    const auto decoded = bd::rom::lz40_decompress(packed, nullptr, Lz40Options{flags, true});
    CHECK(!decoded.ok() || decoded.value() != raw);
  }
  const Bytes long_run = bd_test::lz40_compress(Bytes(70000, 0x33));
  const auto swapped =
      bd::rom::lz40_decompress(long_run, nullptr, Lz40Options{Lz40Options::Flags::Negated, false});
  CHECK(!swapped.ok() || swapped.value() != Bytes(70000, 0x33));
}

TEST(lz40_rejects_bad_streams) {
  CHECK(!bd::rom::lz40_decompress(Bytes{0x40}).ok());
  CHECK(!bd::rom::lz40_decompress(Bytes{0x10, 4, 0, 0, 0, 1, 2, 3, 4}).ok());  // wrong type
  CHECK(!bd::rom::lz40_decompress(Bytes{0x40, 8, 0, 0, 0x00, 1, 2}).ok());     // ends early
  // Distance zero, and a distance before the start of the output.
  CHECK(!bd::rom::lz40_decompress(Bytes{0x40, 8, 0, 0, 0x80, 0x03, 0x00}).ok());
  CHECK(!bd::rom::lz40_decompress(Bytes{0x40, 8, 0, 0, 0x80, 0x53, 0x00}).ok());
  // A back-reference that would overrun the declared size.
  CHECK(!bd::rom::lz40_decompress(Bytes{0x40, 4, 0, 0, 0x40, 'x', 0x1F, 0x00}).ok());
  const Bytes packed = bd_test::lz40_compress(bd_test::sample_data(3000, 13));
  for (std::size_t cut = 0; cut < packed.size(); cut += 5)
    CHECK(!bd::rom::lz40_decompress(bd::ByteSpan(packed).first(cut)).ok());
}
