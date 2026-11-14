#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codecs.h"
#include "core/checksum.h"
#include "nds/arm9_bus.h"
#include "nds/bios.h"
#include "test.h"

namespace {

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

// Main memory, a BIOS on it, and a set of registers.
struct Fixture {
  NoIo io;
  bd::nds::Arm9Bus bus{io};
  bd::nds::Bios bios{bus};
  std::unique_ptr<ds::Cpu> cpu = std::make_unique<ds::Cpu>();  // Large: it holds the call-site list.

  bd::nds::Bios::Result call(std::uint32_t number, std::uint32_t r0 = 0, std::uint32_t r1 = 0,
                             std::uint32_t r2 = 0) {
    cpu->r[0] = r0;
    cpu->r[1] = r1;
    cpu->r[2] = r2;
    return bios.call(*cpu, number);
  }
  void put(std::uint32_t address, const std::vector<std::uint8_t>& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) bus.write8(address + static_cast<std::uint32_t>(i), bytes[i]);
  }
  std::vector<std::uint8_t> get(std::uint32_t address, std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = bus.read8(address + static_cast<std::uint32_t>(i));
    return bytes;
  }
};

constexpr std::uint32_t kA = 0x02001000, kB = 0x02002000;

}  // namespace

TEST(bios_divide) {
  Fixture f;
  CHECK(f.call(0x09, 7, 2).done);
  CHECK(f.cpu->r[0] == 3 && f.cpu->r[1] == 1 && f.cpu->r[3] == 3);
  CHECK(f.call(0x09, static_cast<std::uint32_t>(-7), 2).done);
  CHECK(f.cpu->r[0] == static_cast<std::uint32_t>(-3) && f.cpu->r[1] == static_cast<std::uint32_t>(-1) &&
        f.cpu->r[3] == 3);
  CHECK(f.call(0x09, 7, static_cast<std::uint32_t>(-2)).done);
  CHECK(f.cpu->r[0] == static_cast<std::uint32_t>(-3) && f.cpu->r[1] == 1 && f.cpu->r[3] == 3);
  CHECK(f.call(0x09, 0x80000000u, static_cast<std::uint32_t>(-1)).done);  // The one case that overflows.
  CHECK(f.cpu->r[0] == 0x80000000u && f.cpu->r[1] == 0 && f.cpu->r[3] == 0x80000000u);
  CHECK(f.call(0x09, 0, 5).done);
  CHECK(f.cpu->r[0] == 0 && f.cpu->r[1] == 0 && f.cpu->r[3] == 0);
}

TEST(bios_divide_by_zero_is_reported_not_guessed) {
  Fixture f;
  const auto result = f.call(0x09, 7, 0);
  CHECK(!result.done);
  CHECK(result.problem.find("zero") != std::string::npos);
  CHECK(f.cpu->r[0] == 7);  // Nothing was changed.
}

TEST(bios_square_root) {
  Fixture f;
  const std::uint32_t cases[][2] = {{0, 0},          {1, 1},          {2, 1},          {3, 1},
                                    {4, 2},          {15, 3},         {16, 4},         {17, 4},
                                    {0x3FFFFFFF, 0x7FFF}, {0x40000000, 0x8000}, {0xFFFE0001, 0xFFFF},
                                    {0xFFFE0000, 0xFFFE}, {0xFFFFFFFF, 0xFFFF}};
  for (const auto& c : cases) {
    CHECK(f.call(0x0D, c[0]).done);
    CHECK(f.cpu->r[0] == c[1]);
  }
}

TEST(bios_small_calls) {
  Fixture f;
  CHECK(f.call(0x03, 1000).done);  // WaitByLoop: counted down.
  CHECK(f.cpu->r[0] == 0);
  CHECK(f.call(0x0F, 55).done);    // IsDebugger: no.
  CHECK(f.cpu->r[0] == 0);
}

TEST(bios_cpu_set_copies_and_fills) {
  Fixture f;
  f.put(kA, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  CHECK(f.call(0x0B, kA, kB, 3).done);  // Three 16-bit units.
  CHECK((f.get(kB, 8) == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 0, 0}));
  CHECK(f.call(0x0B, kA + 4, kB + 0x100, 2u | (1u << 26)).done);  // Two 32-bit units.
  CHECK((f.get(kB + 0x100, 10) == std::vector<std::uint8_t>{5, 6, 7, 8, 9, 10, 11, 12, 0, 0}));
  CHECK(f.call(0x0B, kA, kB + 0x200, 3u | (1u << 24)).done);  // Fill with one 16-bit unit.
  CHECK((f.get(kB + 0x200, 8) == std::vector<std::uint8_t>{1, 2, 1, 2, 1, 2, 0, 0}));
  CHECK(f.call(0x0B, kA, kB + 0x300, 2u | (1u << 24) | (1u << 26)).done);  // Fill with one 32-bit unit.
  CHECK((f.get(kB + 0x300, 10) == std::vector<std::uint8_t>{1, 2, 3, 4, 1, 2, 3, 4, 0, 0}));
  CHECK(f.cpu->r[0] == kA && f.cpu->r[1] == kB + 0x300);  // The arguments are left alone.
}

TEST(bios_cpu_set_details) {
  Fixture f;
  f.put(kA, {1, 2, 3, 4, 5, 6, 7, 8});
  // Addresses are forced to the unit's alignment.
  CHECK(f.call(0x0B, kA + 1, kB + 1, 1).done);
  CHECK((f.get(kB, 3) == std::vector<std::uint8_t>{1, 2, 0}));
  CHECK(f.call(0x0B, kA + 3, kB + 0x103, 1u | (1u << 26)).done);
  CHECK((f.get(kB + 0x100, 5) == std::vector<std::uint8_t>{1, 2, 3, 4, 0}));
  // Bits of the mode word other than the count and the two mode bits do not matter.
  CHECK(f.call(0x0B, kA, kB + 0x200, 1u | (1u << 26) | 0xFA000000u).done);
  CHECK((f.get(kB + 0x200, 5) == std::vector<std::uint8_t>{1, 2, 3, 4, 0}));
  // A count of zero does nothing.
  CHECK(f.call(0x0B, kA, kB + 0x300, 1u << 26).done);
  CHECK((f.get(kB + 0x300, 4) == std::vector<std::uint8_t>{0, 0, 0, 0}));
  // A fill reads its source once: filling over the source itself spreads the
  // original value, not what the fill wrote on the way.
  f.put(kB + 0x400, {0, 0, 0, 0, 9, 9, 9, 9, 0, 0, 0, 0});
  CHECK(f.call(0x0B, kB + 0x404, kB + 0x400, 3u | (1u << 24) | (1u << 26)).done);
  CHECK((f.get(kB + 0x400, 12) == std::vector<std::uint8_t>{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}));
}

TEST(bios_cpu_fast_set) {
  Fixture f;
  f.put(kA, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  CHECK(f.call(0x0C, kA, kB, 3).done);
  CHECK((f.get(kB, 14) == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 0}));
  CHECK(f.call(0x0C, kA + 4, kB + 0x100, 2u | (1u << 24)).done);
  CHECK((f.get(kB + 0x100, 10) == std::vector<std::uint8_t>{5, 6, 7, 8, 5, 6, 7, 8, 0, 0}));
}

TEST(bios_crc16) {
  Fixture f;
  const std::vector<std::uint8_t> data = bd_test::sample_data(256, 7);
  f.put(kA, data);
  // Against the project's other implementation, which the ROM header checks use.
  CHECK(f.call(0x0E, 0xFFFF, kA, 256).done);
  CHECK(f.cpu->r[0] == bd::crc16(bd::ByteSpan(data.data(), data.size())));
  // Against a published check value: "123456789" gives 0xBB3D from an
  // initial value of 0. The call wants an even length, so the last byte is
  // put through one step by hand here.
  f.put(kB, {'1', '2', '3', '4', '5', '6', '7', '8'});
  CHECK(f.call(0x0E, 0, kB, 8).done);
  std::uint32_t crc = f.cpu->r[0] ^ '9';
  for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xA001u : 0u);
  CHECK(crc == 0xBB3D);
  // In two parts, carrying the value over, gives the same as in one.
  CHECK(f.call(0x0E, 0xFFFF, kA, 100).done);
  CHECK(f.call(0x0E, f.cpu->r[0], kA + 100, 156).done);
  CHECK(f.cpu->r[0] == bd::crc16(bd::ByteSpan(data.data(), data.size())));
  // Length zero returns the initial value.
  CHECK(f.call(0x0E, 0x1234, kA, 0).done);
  CHECK(f.cpu->r[0] == 0x1234);
  CHECK(!f.call(0x0E, 0, kA, 3).done);  // Odd: not known, so not guessed.
}

TEST(bios_lz77) {
  Fixture f;
  // Compressible data with long repeats, and data with none.
  std::vector<std::uint8_t> repetitive;
  for (int i = 0; i < 3000; ++i) repetitive.push_back(static_cast<std::uint8_t>("abcabcabcd"[i % 10] + (i / 700)));
  for (const std::vector<std::uint8_t>& raw : {repetitive, bd_test::sample_data(777, 3), std::vector<std::uint8_t>{42}}) {
    const std::vector<std::uint8_t> packed = bd_test::lz10_compress(raw);
    f.put(kA, packed);
    f.put(kB, std::vector<std::uint8_t>(raw.size() + 8, 0xEE));
    CHECK(f.call(0x11, kA, kB).done);
    CHECK(f.get(kB, raw.size()) == raw);
    // Nothing beyond the declared size is written.
    CHECK((f.get(kB + static_cast<std::uint32_t>(raw.size()), 8) == std::vector<std::uint8_t>(8, 0xEE)));
  }
}

TEST(bios_run_length) {
  Fixture f;
  // Header: type 0x30, size 12. Then: 5 times 'A' (flag 0x82), three literal
  // bytes (flag 0x02), 4 times 'Z' (flag 0x81).
  f.put(kA, {0x30, 12, 0, 0, 0x82, 'A', 0x02, 'x', 'y', 'z', 0x81, 'Z'});
  f.put(kB, std::vector<std::uint8_t>(16, 0xEE));
  CHECK(f.call(0x14, kA, kB).done);
  CHECK((f.get(kB, 16) == std::vector<std::uint8_t>{'A', 'A', 'A', 'A', 'A', 'x', 'y', 'z', 'Z', 'Z', 'Z', 'Z', 0xEE,
                                                    0xEE, 0xEE, 0xEE}));
  // A run that would go past the declared size is cut off there.
  f.put(kA, {0x30, 2, 0, 0, 0x85, 'Q'});
  f.put(kB, std::vector<std::uint8_t>(4, 0xEE));
  CHECK(f.call(0x14, kA, kB).done);
  CHECK((f.get(kB, 4) == std::vector<std::uint8_t>{'Q', 'Q', 0xEE, 0xEE}));
}

TEST(bios_calls_that_are_not_implemented_say_so) {
  Fixture f;
  const auto halt = f.call(0x06);
  CHECK(!halt.done && halt.problem.find("Halt") != std::string::npos);
  const auto unknown = f.call(0x2A);
  CHECK(!unknown.done && unknown.problem.find("unknown number") != std::string::npos);
  CHECK(std::string(bd::nds::Bios::name(0x0B)) == "CpuSet");
  CHECK(bd::nds::Bios::name(0x2A) == nullptr);
}
