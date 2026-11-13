// The system coprocessor of the DS main processor (CP15 of an ARM946E-S).
//
// Programs use it to place and enable the two small fast memories, to set up
// memory protection and caches, and to wait for an interrupt. Here it places
// the fast memories in the memory map; protection and caches are not
// modelled, so those registers are stored and nothing more.
#pragma once

#include <cstdint>
#include <map>

#include "nds/arm9_bus.h"

namespace bd::nds {

class SystemControl {
 public:
  explicit SystemControl(Arm9Bus& bus);

  // Register addresses are (CRn, CRm, opcode2), as in "mrc p15, 0, r0, c9,
  // c1, 0"; opcode1 is 0 for every register this processor has.
  std::uint32_t read(int crn, int crm, int opc2);
  void write(int crn, int crm, int opc2, std::uint32_t value);

  // Sets the registers the way the console's firmware leaves them when it
  // starts a game.
  void set_firmware_state();

  // Whether the program has asked to wait for an interrupt since this was
  // last cleared.
  bool wait_requested() const { return wait_requested_; }
  void clear_wait_request() { wait_requested_ = false; }

  // Exception vectors at 0xFFFF0000 (true) or 0x00000000.
  bool high_vectors() const { return (control_ & (1u << 13)) != 0; }
  std::uint32_t control() const { return control_; }

  // Accesses to registers this model does not know, by (CRn << 8 | CRm << 4
  // | opcode2).
  const std::map<std::uint32_t, std::uint64_t>& unknown_reads() const { return unknown_reads_; }
  const std::map<std::uint32_t, std::uint64_t>& unknown_writes() const { return unknown_writes_; }

 private:
  void apply();

  Arm9Bus& bus_;
  std::uint32_t control_ = 0x00000078;  // Its value at reset.
  std::uint32_t dtcm_ = 0, itcm_ = 0;
  std::uint32_t cache_config_[2] = {}, write_buffer_ = 0;
  std::uint32_t access_[4] = {}, region_[8] = {};
  std::uint32_t process_id_ = 0;
  bool wait_requested_ = false;
  std::map<std::uint32_t, std::uint64_t> unknown_reads_, unknown_writes_;
};

}  // namespace bd::nds
