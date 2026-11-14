#include <cstdint>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/dma.h"
#include "nds/interrupts.h"
#include "test.h"

namespace {

// Memory, registers and the DMA channels, wired as in the console.
struct Fixture {
  bd::nds::Arm9Io io;
  bd::nds::Arm9Bus bus{io};
  bd::nds::Interrupts irq;
  bd::nds::Dma dma{bus, irq};
  Fixture() {
    irq.attach(io);
    dma.attach(io);
  }
  // Starts channel `n` the way a program does: source, destination, control.
  void start(int n, std::uint32_t source, std::uint32_t destination, std::uint32_t control) {
    const std::uint32_t base = 0x040000B0 + 12 * static_cast<std::uint32_t>(n);
    bus.write32(base, source);
    bus.write32(base + 4, destination);
    bus.write32(base + 8, control);
  }
  std::uint32_t control(int n) { return bus.read32(0x040000B8 + 12 * static_cast<std::uint32_t>(n)); }
};

constexpr std::uint32_t kEnable = 1u << 31, kWords = 1u << 26, kIrq = 1u << 30;
constexpr std::uint32_t kSourceFixed = 2u << 23, kSourceDown = 1u << 23;
constexpr std::uint32_t kDestFixed = 2u << 21, kDestDown = 1u << 21;
constexpr std::uint32_t kA = 0x02001000, kB = 0x02002000;

}  // namespace

TEST(dma_copies_at_once_and_clears_its_enable_bit) {
  Fixture f;
  for (std::uint32_t i = 0; i < 4; ++i) f.bus.write32(kA + 4 * i, 0x1000 + i);
  f.start(0, kA, kB, kEnable | kWords | 3);
  CHECK(f.bus.read32(kB) == 0x1000 && f.bus.read32(kB + 4) == 0x1001 && f.bus.read32(kB + 8) == 0x1002);
  CHECK(f.bus.read32(kB + 12) == 0);  // Three units, not four.
  CHECK(f.control(0) == (kWords | 3));  // Finished: the enable bit is clear, the rest stays.
  CHECK(f.dma.statistics().transfers == 1 && f.dma.statistics().units == 3);
  CHECK(f.irq.requested() == 0);
  // The address registers read back what was written, not where the transfer ended.
  CHECK(f.bus.read32(0x040000B0) == kA && f.bus.read32(0x040000B4) == kB);
}

TEST(dma_sixteen_bit_units) {
  Fixture f;
  f.bus.write32(kA, 0x44332211);
  f.bus.write32(kA + 4, 0x88776655);
  f.start(1, kA, kB, kEnable | 3);
  CHECK(f.bus.read32(kB) == 0x44332211);
  CHECK(f.bus.read32(kB + 4) == 0x00006655);
}

TEST(dma_fills_from_a_fixed_source) {
  Fixture f;
  // The usual way to fill: the value goes into the channel's fill register,
  // and that register is the fixed source.
  f.bus.write32(0x040000EC, 0xCAFEF00D);
  f.start(3, 0x040000EC, kB, kEnable | kWords | kSourceFixed | 4);
  for (std::uint32_t i = 0; i < 4; ++i) CHECK(f.bus.read32(kB + 4 * i) == 0xCAFEF00D);
  CHECK(f.bus.read32(kB + 16) == 0);
  CHECK(f.bus.read32(0x040000EC) == 0xCAFEF00D);
  CHECK(f.io.unmodelled().empty());
}

TEST(dma_can_write_hardware_registers) {
  Fixture f;
  // What the game does at start-up: clear a run of registers with a fill.
  f.bus.write32(0x04000008, 0x11111111);
  f.bus.write32(0x0400000C, 0x22222222);
  f.bus.write32(0x040000E0, 0);
  f.start(0, 0x040000E0, 0x04000008, kEnable | kWords | kSourceFixed | 2);
  CHECK(f.bus.read32(0x04000008) == 0 && f.bus.read32(0x0400000C) == 0);
  CHECK(f.control(0) == (kWords | kSourceFixed | 2));
}

TEST(dma_directions) {
  Fixture f;
  for (std::uint32_t i = 0; i < 4; ++i) f.bus.write32(kA + 4 * i, 0x10 + i);
  f.start(0, kA + 12, kB, kEnable | kWords | kSourceDown | 4);  // Source downwards.
  CHECK(f.bus.read32(kB) == 0x13 && f.bus.read32(kB + 12) == 0x10);
  f.start(0, kA, kB + 0x10C, kEnable | kWords | kDestDown | 4);  // Destination downwards.
  CHECK(f.bus.read32(kB + 0x10C) == 0x10 && f.bus.read32(kB + 0x100) == 0x13);
  f.start(0, kA, kB + 0x200, kEnable | kWords | kDestFixed | 4);  // Destination fixed: the last one stays.
  CHECK(f.bus.read32(kB + 0x200) == 0x13 && f.bus.read32(kB + 0x204) == 0);
}

TEST(dma_interrupt_on_completion) {
  Fixture f;
  f.start(2, kA, kB, kEnable | kIrq | 1);
  CHECK(f.irq.requested() == (1u << 10));  // Channel 2.
  f.start(0, kA, kB, kEnable | kIrq | 1);
  CHECK(f.irq.requested() == ((1u << 10) | (1u << 8)));
}

TEST(dma_starts_on_the_enable_bit_going_up) {
  Fixture f;
  f.bus.write32(kA, 7);
  f.bus.write32(0x040000B0, kA);
  f.bus.write32(0x040000B4, kB);
  f.bus.write32(0x040000B8, kWords | 1);  // Set up, not enabled: nothing moves.
  CHECK(f.bus.read32(kB) == 0 && f.dma.statistics().transfers == 0);
  f.bus.write8(0x040000BB, 0x84);  // Only the top byte: enable (and the word-size bit again).
  CHECK(f.bus.read32(kB) == 7 && f.dma.statistics().transfers == 1);
}

TEST(dma_a_count_of_zero_is_the_maximum) {
  Fixture f;
  f.bus.write16(kA, 0xABCD);
  // 0x200000 16-bit units from a fixed source: all 4 MiB of main memory.
  f.start(0, kA, 0x02000000, kEnable | kSourceFixed);
  CHECK(f.dma.statistics().units == 0x200000);
  CHECK(f.bus.read16(0x02000000) == 0xABCD && f.bus.read16(0x023FFFFE) == 0xABCD);
}

TEST(dma_transfers_that_wait_for_an_event_stay_pending) {
  Fixture f;
  f.bus.write32(kA, 7);
  f.start(1, kA, kB, kEnable | kWords | (1u << 27) | 1);  // Start at vertical blank.
  CHECK(f.bus.read32(kB) == 0);
  CHECK((f.control(1) & kEnable) != 0);  // Still waiting.
  CHECK(f.dma.statistics().waiting_for_event[1] == 1 && f.dma.statistics().transfers == 0);
}

TEST(dma_addresses_are_28_bits) {
  Fixture f;
  f.bus.write32(0x040000B0, 0xF2001000);
  CHECK(f.bus.read32(0x040000B0) == 0x02001000);
}
