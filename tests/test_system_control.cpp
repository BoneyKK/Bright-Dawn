#include "nds/system_control.h"
#include "test.h"

namespace {

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

}  // namespace

TEST(system_control_identifies_the_processor) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  CHECK(cp.read(0, 0, 0) == 0x41059461);
  CHECK(cp.read(0, 0, 1) == 0x0F0D2112);
  CHECK(cp.read(0, 0, 2) == 0x00140180);
  CHECK(cp.unknown_reads().empty());
}

TEST(system_control_control_register_has_fixed_bits) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  CHECK(cp.read(1, 0, 0) == 0x00000078);
  cp.write(1, 0, 0, 0xFFFFFFFF);
  CHECK(cp.read(1, 0, 0) == 0x000FF0FD);
  cp.write(1, 0, 0, 0);
  CHECK(cp.read(1, 0, 0) == 0x00000078);
  CHECK(!cp.high_vectors());
  cp.write(1, 0, 0, 1u << 13);
  CHECK(cp.high_vectors());
}

TEST(system_control_firmware_state_maps_both_fast_memories) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.set_firmware_state();
  CHECK(cp.read(1, 0, 0) == 0x0005707D);
  CHECK(cp.high_vectors());
  bus.write32(0x01FF8000, 0x11);  // Instruction memory, anywhere below 32 MiB.
  CHECK(bus.read32(0x00000000) == 0x11);
  bus.write32(0x03000000, 0x22);  // Data memory, in place of the shared memory there.
  CHECK(bus.read32(0x03000000) == 0x22);
  CHECK(bus.read32(0x03004000) == 0);  // Past its 16 KiB: shared memory.
  CHECK(bus.statistics().unmapped_writes.empty());
}

TEST(system_control_program_moves_the_data_memory) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.set_firmware_state();
  bus.write32(0x03000000, 0x22);
  cp.write(9, 1, 0, 0x027E000A);  // What DS programs set: 16 KiB at 0x027E0000.
  CHECK(cp.read(9, 1, 0) == 0x027E000A);
  CHECK(bus.read32(0x027E0000) == 0x22);  // The same memory, at its new address.
  CHECK(bus.read32(0x03000000) == 0);
}

TEST(system_control_load_mode_is_write_only) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.write(9, 1, 0, 0x027E000A);
  bus.write32(0x027E0000, 0x11);                  // Main memory: the data memory is off.
  cp.write(1, 0, 0, (1u << 16) | (1u << 17));     // On, in load mode.
  bus.write32(0x027E0000, 0x22);
  CHECK(bus.read32(0x027E0000) == 0x11);
  cp.write(1, 0, 0, 1u << 16);
  CHECK(bus.read32(0x027E0000) == 0x22);
  cp.write(1, 0, 0, 0);
  CHECK(bus.read32(0x027E0000) == 0x11);
}

TEST(system_control_stores_protection_registers) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.write(6, 3, 0, 0x0200002B);
  cp.write(5, 0, 2, 0x33333333);
  cp.write(2, 0, 1, 0x42);
  cp.write(3, 0, 0, 0x02);
  CHECK(cp.read(6, 3, 0) == 0x0200002B);
  CHECK(cp.read(5, 0, 2) == 0x33333333);
  CHECK(cp.read(2, 0, 1) == 0x42);
  CHECK(cp.read(3, 0, 0) == 0x02);
  CHECK(cp.unknown_writes().empty() && cp.unknown_reads().empty());
}

TEST(system_control_cache_maintenance_does_nothing) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.write(7, 5, 0, 0);
  cp.write(7, 6, 0, 0);
  cp.write(7, 10, 4, 0);
  cp.write(7, 14, 1, 0x02000000);
  CHECK(cp.unknown_writes().empty());
  CHECK(!cp.wait_requested());
}

TEST(system_control_wait_for_interrupt) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  cp.write(7, 0, 4, 0);
  CHECK(cp.wait_requested());
  cp.clear_wait_request();
  CHECK(!cp.wait_requested());
  cp.write(7, 8, 2, 0);
  CHECK(cp.wait_requested());
}

TEST(system_control_counts_unknown_registers) {
  NoIo io;
  bd::nds::Arm9Bus bus(io);
  bd::nds::SystemControl cp(bus);
  CHECK(cp.read(15, 0, 0) == 0);
  cp.write(15, 1, 0, 1);
  CHECK(cp.unknown_reads().at(0xF00) == 1);
  CHECK(cp.unknown_writes().at(0xF10) == 1);
}
