#include <cstdint>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/cartridge.h"
#include "nds/dma.h"
#include "nds/interrupts.h"
#include "test.h"

namespace {

using bd::nds::Cartridge;
using bd::nds::Interrupts;

// A cartridge whose every 32-bit word holds its own address.
std::vector<std::uint8_t> numbered_rom(std::uint32_t size) {
  std::vector<std::uint8_t> rom(size);
  for (std::uint32_t a = 0; a + 4 <= size; a += 4) {
    for (int b = 0; b < 4; ++b) rom[a + b] = static_cast<std::uint8_t>(a >> (8 * b));
  }
  return rom;
}

// Memory, registers, DMA and the cartridge slot, wired as in the console.
struct Fixture {
  bd::nds::Arm9Io io;
  bd::nds::Arm9Bus bus{io};
  Interrupts irq;
  bd::nds::Dma dma{bus, irq};
  Cartridge card{irq};
  std::vector<std::uint8_t> rom = numbered_rom(0x20000);
  explicit Fixture(bool with_cartridge = true) {
    irq.attach(io);
    dma.attach(io);
    card.attach(io);
    card.on_word_ready = [this] { return dma.trigger(bd::nds::Dma::kStartCartridge); };
    dma.on_waiting = [this](unsigned mode) {
      if (mode == bd::nds::Dma::kStartCartridge) card.pump();
    };
    if (with_cartridge) card.insert(bd::ByteSpan(rom.data(), rom.size()));
  }
  // Sends "read data at `address`" (or another command byte) and starts a
  // transfer with block size code `size`.
  void start(std::uint32_t address, unsigned size, std::uint8_t command = 0xB7) {
    bus.write8(0x040001A8, command);
    bus.write8(0x040001A9, static_cast<std::uint8_t>(address >> 24));
    bus.write8(0x040001AA, static_cast<std::uint8_t>(address >> 16));
    bus.write8(0x040001AB, static_cast<std::uint8_t>(address >> 8));
    bus.write8(0x040001AC, static_cast<std::uint8_t>(address));
    bus.write8(0x040001AD, 0);
    bus.write8(0x040001AE, 0);
    bus.write8(0x040001AF, 0);
    bus.write32(0x040001A4, 0x80000000u | (static_cast<std::uint32_t>(size) << 24));
  }
  std::uint32_t control() { return bus.read32(0x040001A4); }
  std::uint32_t data() { return bus.read32(0x04100010); }
};

constexpr std::uint32_t kBusy = 1u << 31, kReady = 1u << 23;

}  // namespace

TEST(cartridge_reads_a_block_word_by_word) {
  Fixture f;
  f.start(0x00010000, 1);  // 0x200 bytes.
  CHECK((f.control() & kBusy) != 0 && (f.control() & kReady) != 0);
  bool right = true;
  for (std::uint32_t i = 0; i < 0x200 / 4; ++i) {
    right = right && (f.control() & kReady) != 0;
    right = right && f.data() == 0x00010000 + 4 * i;
  }
  CHECK(right);
  CHECK((f.control() & (kBusy | kReady)) == 0);  // Done.
  CHECK(f.data() == 0);                          // Nothing more comes.
  CHECK(f.irq.requested() == 0);                 // The completion interrupt was not asked for.
  const auto& s = f.card.statistics();
  CHECK(s.data_reads == 1 && s.data_bytes == 0x200);
  CHECK(s.lowest_address == 0x00010000 && s.highest_address == 0x000101FF);
  CHECK(f.io.unmodelled().empty());
}

TEST(cartridge_block_sizes) {
  Fixture f;
  f.start(0x00010000, 7);  // Four bytes.
  CHECK(f.data() == 0x00010000);
  CHECK((f.control() & kBusy) == 0);
  f.start(0x00010000, 2);  // 0x400 bytes.
  for (int i = 0; i < 0x400 / 4 - 1; ++i) (void)f.data();
  CHECK((f.control() & kBusy) != 0);
  CHECK(f.data() == 0x000103FC);
  CHECK((f.control() & kBusy) == 0);
  f.start(0x00010000, 0);  // No data: over at once.
  CHECK((f.control() & (kBusy | kReady)) == 0);
  CHECK(f.card.statistics().data_bytes == 4 + 0x400);
}

TEST(cartridge_completion_interrupt) {
  Fixture f;
  f.bus.write16(0x040001A0, 0xC000);  // Slot on, interrupt when a block has been read.
  CHECK(f.bus.read16(0x040001A0) == 0xC000);
  f.start(0x00010000, 7);
  CHECK(f.irq.requested() == 0);  // Not until the word is read.
  (void)f.data();
  CHECK(f.irq.requested() == (1u << Interrupts::kCartridgeTransfer));
  f.bus.write32(0x04000214, 0xFFFFFFFF);
  f.start(0x00010000, 0);  // A transfer with no data completes at once.
  CHECK(f.irq.requested() == (1u << Interrupts::kCartridgeTransfer));
}

TEST(cartridge_chip_identifier) {
  Fixture f;
  f.start(0, 7, 0xB8);
  CHECK(f.data() == Cartridge::kChipId);
  CHECK(f.card.statistics().chip_id_reads == 1 && f.card.statistics().data_reads == 0);
}

TEST(cartridge_first_32_kib_cannot_be_read) {
  Fixture f;
  // An address below 0x8000 reads from the 512 bytes that follow that area.
  f.start(0x00000000, 7);
  CHECK(f.data() == 0x00008000);
  f.start(0x00004324, 7);
  CHECK(f.data() == 0x00008124);
  f.start(0x00007FFC, 1);  // A block that crosses the boundary: each word by its own address.
  CHECK(f.data() == 0x000081FC);
  CHECK(f.data() == 0x00008000);
  CHECK(f.data() == 0x00008004);
}

TEST(cartridge_reads_beyond_the_end_and_from_an_empty_slot) {
  Fixture f;
  f.start(0x0001FFFC, 1);
  CHECK(f.data() == 0x0001FFFC);         // The last word there is.
  CHECK(f.data() == Cartridge::kNoData);  // Past the end.
  CHECK(f.card.statistics().reads_beyond_the_end == 1);

  Fixture empty(false);
  empty.start(0x00010000, 7);
  CHECK(empty.data() == Cartridge::kNoData);
  CHECK(empty.card.statistics().reads_without_cartridge == 1);
  CHECK((empty.control() & kBusy) == 0);  // The transfer itself still completes.
}

TEST(cartridge_other_commands_are_counted) {
  Fixture f;
  f.start(0, 7, 0x9F);
  CHECK(f.data() == Cartridge::kNoData);
  f.start(0, 7, 0x3C);
  (void)f.data();
  f.start(0, 7, 0x9F);
  (void)f.data();
  CHECK(f.card.statistics().other_commands.at(0x9F) == 2);
  CHECK(f.card.statistics().other_commands.at(0x3C) == 1);
}

TEST(cartridge_command_register_reads_back) {
  Fixture f;
  // The command's first byte is at the lowest address. Written as two
  // words: B7 00 01 00, then 04 00 00 00, which is "read data at 0x00010004".
  f.bus.write32(0x040001A8, 0x000100B7);
  f.bus.write32(0x040001AC, 0x00000004);
  CHECK(f.bus.read32(0x040001A8) == 0x000100B7 && f.bus.read32(0x040001AC) == 0x00000004);
  f.bus.write32(0x040001A4, 0x80000000u | (7u << 24));
  CHECK(f.data() == 0x00010004);
}

TEST(cartridge_feeds_a_dma_channel_one_word_at_a_time) {
  Fixture f;
  // What DS programs do: channel 3 takes one word per word delivered, from
  // the data register to memory, and stays enabled for the next.
  f.bus.write32(0x040000D4, 0x04100010);
  f.bus.write32(0x040000D8, 0x02001000);
  f.bus.write32(0x040000DC, 0x80000000u | (1u << 26) | (2u << 23) | (1u << 25) | (5u << 27) | 1);
  CHECK(f.dma.statistics().transfers == 0);
  f.start(0x00010000, 1);
  CHECK((f.control() & (kBusy | kReady)) == 0);  // The whole block went at once.
  bool right = true;
  for (std::uint32_t i = 0; i < 0x200 / 4; ++i) right = right && f.bus.read32(0x02001000 + 4 * i) == 0x00010000 + 4 * i;
  CHECK(right);
  CHECK(f.bus.read32(0x02001200) == 0);
  CHECK(f.dma.statistics().transfers == 0x200 / 4);
  CHECK((f.bus.read32(0x040000DC) & 0x80000000u) != 0);  // Still enabled: it repeats.
}

TEST(cartridge_dma_channel_enabled_after_the_transfer_began) {
  Fixture f;
  f.start(0x00010000, 7);
  CHECK((f.control() & kReady) != 0);
  f.bus.write32(0x040000D4, 0x04100010);
  f.bus.write32(0x040000D8, 0x02001000);
  f.bus.write32(0x040000DC, 0x80000000u | (1u << 26) | (2u << 23) | (5u << 27) | 1);
  CHECK(f.bus.read32(0x02001000) == 0x00010000);  // The waiting word was taken at once.
  CHECK((f.control() & kBusy) == 0);
  CHECK((f.bus.read32(0x040000DC) & 0x80000000u) == 0);  // Not set to repeat: finished.
}

TEST(dma_trigger_moves_only_channels_waiting_for_that_event) {
  Fixture f;
  f.bus.write32(0x02000100, 0x1234);
  f.bus.write32(0x040000B0, 0x02000100);
  f.bus.write32(0x040000B4, 0x02000200);
  f.bus.write32(0x040000B8, 0x80000000u | (1u << 26) | (1u << 27) | 1);  // Waits for the vertical blank.
  CHECK(!f.dma.trigger(bd::nds::Dma::kStartCartridge));
  CHECK(f.bus.read32(0x02000200) == 0);
  CHECK(f.dma.trigger(1));
  CHECK(f.bus.read32(0x02000200) == 0x1234);
  CHECK(!f.dma.trigger(1));  // Finished: it was not set to repeat.
}

TEST(dma_repeating_transfer_goes_on_from_where_it_was) {
  Fixture f;
  for (std::uint32_t i = 0; i < 4; ++i) f.bus.write32(0x02000100 + 4 * i, 0x50 + i);
  f.bus.write32(0x040000B0, 0x02000100);
  f.bus.write32(0x040000B4, 0x02000200);
  f.bus.write32(0x040000B8, 0x80000000u | (1u << 26) | (1u << 25) | (1u << 27) | 2);  // Two words per event.
  CHECK(f.dma.trigger(1));
  CHECK(f.dma.trigger(1));
  for (std::uint32_t i = 0; i < 4; ++i) CHECK(f.bus.read32(0x02000200 + 4 * i) == 0x50 + i);
  // Destination mode 3 starts each round at the destination again.
  f.bus.write32(0x040000B8, 0);
  f.bus.write32(0x040000B8, 0x80000000u | (1u << 26) | (1u << 25) | (3u << 21) | (1u << 27) | 1);
  CHECK(f.dma.trigger(1));
  CHECK(f.dma.trigger(1));
  CHECK(f.bus.read32(0x02000200) == 0x51 && f.bus.read32(0x02000204) == 0x51);
}
