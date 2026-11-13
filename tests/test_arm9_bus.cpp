#include <vector>

#include "nds/arm9_bus.h"
#include "test.h"

namespace {

// Records what reaches the hardware registers.
class RecordingIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t address, unsigned size) override {
    last_address = address;
    last_size = size;
    ++reads;
    return 0xA5A5A5A5u;
  }
  void write(std::uint32_t address, unsigned size, std::uint32_t value) override {
    last_address = address;
    last_size = size;
    last_value = value;
    ++writes;
  }
  std::uint32_t last_address = 0, last_value = 0;
  unsigned last_size = 0;
  int reads = 0, writes = 0;
};

}  // namespace

TEST(arm9_bus_main_memory_repeats_every_4_mib) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write32(0x02000010, 0x11223344);
  CHECK(bus.read32(0x02000010) == 0x11223344);
  CHECK(bus.read32(0x02400010) == 0x11223344);
  CHECK(bus.read32(0x02C00010) == 0x11223344);
  CHECK(bus.read16(0x02000010) == 0x3344);
  CHECK(bus.read16(0x02000012) == 0x1122);
  CHECK(bus.read8(0x02000013) == 0x11);
  bus.write8(0x023FFFFF, 0x7E);
  CHECK(bus.read8(0x02FFFFFF) == 0x7E);
  CHECK(io.reads == 0 && io.writes == 0);
}

TEST(arm9_bus_aligns_wide_accesses) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write32(0x02000103, 0xCAFEBABE);  // Lands on 0x02000100.
  CHECK(bus.read32(0x02000100) == 0xCAFEBABE);
  bus.write16(0x02000201, 0xBEEF);      // Lands on 0x02000200.
  CHECK(bus.read16(0x02000200) == 0xBEEF);
}

TEST(arm9_bus_fast_memories_are_absent_until_mapped) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write32(0x01FF8000, 0x12345678);
  CHECK(bus.read32(0x01FF8000) == 0);
  CHECK(bus.statistics().unmapped_writes.at(0x01FF) == 1);
  CHECK(bus.statistics().unmapped_reads.at(0x01FF) == 1);

  bus.map_itcm(true, true, 0x02000000);
  bus.write32(0x01FF8000, 0x12345678);
  CHECK(bus.read32(0x01FF8000) == 0x12345678);
  CHECK(bus.read32(0x00000000) == 0x12345678);  // 32 KiB, repeating from address 0.
  CHECK(bus.read32(0x01000000) == 0x12345678);
  CHECK(bus.read32(0x02000000) == 0);            // Main memory is something else.
}

TEST(arm9_bus_data_memory_takes_precedence) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write32(0x027E0000, 0xAAAAAAAA);  // Main memory, mirrored, before mapping.
  bus.map_dtcm(true, true, 0x027E0000, 0x4000);
  CHECK(bus.read32(0x027E0000) == 0);   // Now the data memory, which is empty.
  bus.write32(0x027E0000, 0xBBBBBBBB);
  bus.write32(0x027E3FFC, 0xCCCCCCCC);
  CHECK(bus.read32(0x027E0000) == 0xBBBBBBBB);
  CHECK(bus.read32(0x027E3FFC) == 0xCCCCCCCC);
  CHECK(bus.read32(0x023E0000) == 0xAAAAAAAA);  // The main memory underneath is untouched.
  CHECK(bus.read32(0x027E4000) == 0);            // Just past its 16 KiB: main memory again.
  bus.map_dtcm(false, false, 0, 0);
  CHECK(bus.read32(0x027E0000) == 0xAAAAAAAA);
}

TEST(arm9_bus_data_memory_repeats_within_a_larger_region) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.map_dtcm(true, true, 0x03000000, 0x01000000);  // As the console's firmware leaves it.
  bus.write32(0x03000010, 0x600DF00D);
  CHECK(bus.read32(0x03004010) == 0x600DF00D);
  CHECK(bus.read32(0x03FFC010) == 0x600DF00D);
  CHECK(bus.read32(0x02FFC010) == 0);  // Below the region: main memory.
}

TEST(arm9_bus_fast_memory_being_filled_is_write_only) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write32(0x027E0000, 0x11111111);            // Main memory.
  bus.map_dtcm(false, true, 0x027E0000, 0x4000);  // "Load mode".
  bus.write32(0x027E0000, 0x22222222);            // Goes to the data memory...
  CHECK(bus.read32(0x027E0000) == 0x11111111);    // ...while reads still see main memory.
  bus.map_dtcm(true, true, 0x027E0000, 0x4000);
  CHECK(bus.read32(0x027E0000) == 0x22222222);
}

TEST(arm9_bus_instruction_memory_wins_over_data_memory) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.map_itcm(true, true, 0x02000000);
  bus.map_dtcm(true, true, 0x01000000, 0x4000);
  bus.write32(0x01000000, 0x1234);
  bus.map_dtcm(false, false, 0, 0);
  CHECK(bus.read32(0x01000000) == 0x1234);  // It went to the instruction memory.
  CHECK(bus.read32(0x00000000) == 0x1234);
}

TEST(arm9_bus_fast_memory_can_cover_hardware_registers) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.map_dtcm(true, true, 0x04000000, 0x4000);
  bus.write32(0x04000000, 0x77);
  CHECK(bus.read32(0x04000000) == 0x77);
  CHECK(io.reads == 0 && io.writes == 0);
  CHECK(bus.read32(0x04004000) == 0xA5A5A5A5u);  // Past it: the registers again.
  CHECK(io.reads == 1);
}

TEST(arm9_bus_routes_hardware_registers) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  CHECK(bus.read16(0x04000006) == 0xA5A5);
  CHECK(io.last_address == 0x04000006 && io.last_size == 2);
  bus.write32(0x04000208, 1);
  CHECK(io.last_address == 0x04000208 && io.last_size == 4 && io.last_value == 1);
  bus.write8(0x04000300, 2);
  CHECK(io.last_address == 0x04000300 && io.last_size == 1 && io.last_value == 2);
  CHECK(io.reads == 1 && io.writes == 2);
}

TEST(arm9_bus_small_regions_repeat) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  bus.write16(0x05000000, 0x7FFF);
  CHECK(bus.read16(0x05000800) == 0x7FFF);  // Palette: 2 KiB.
  bus.write32(0x07000004, 0x01020304);
  CHECK(bus.read32(0x07000804) == 0x01020304);  // Sprite table: 2 KiB.
  bus.write32(0x03000000, 0x99);
  CHECK(bus.read32(0x03008000) == 0x99);    // Shared memory: 32 KiB.
  bus.write32(0x06800000, 0x55);
  CHECK(bus.read32(0x06800000) == 0x55);
}

TEST(arm9_bus_counts_unmapped_accesses) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  CHECK(bus.read32(0xFFFF0000) == 0);
  CHECK(bus.read8(0x08000000) == 0);
  bus.write32(0x0A000000, 1);
  CHECK(bus.statistics().unmapped_reads.at(0xFFFF) == 1);
  CHECK(bus.statistics().unmapped_reads.at(0x0800) == 1);
  CHECK(bus.statistics().unmapped_writes.at(0x0A00) == 1);
}

TEST(arm9_bus_load_checks_its_range) {
  RecordingIo io;
  bd::nds::Arm9Bus bus(io);
  const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(bus.load(0x02000000, data.data(), data.size()));
  CHECK(bus.read32(0x02000004) == 0x08070605);
  CHECK(bus.load(0x023FFFF8, data.data(), data.size()));
  CHECK(!bus.load(0x023FFFFC, data.data(), data.size()));  // Would run past the end.
  CHECK(!bus.load(0x01FF8000, data.data(), data.size()));
  CHECK(!bus.load(0x03000000, data.data(), data.size()));
}
