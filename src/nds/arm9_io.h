// The hardware registers of the DS main processor.
//
// Almost nothing is modelled yet. A register without a model keeps the value
// last written to it and every access is counted, so that a run of the game's
// code shows which registers it needs and in what order of importance.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "nds/arm9_bus.h"

namespace bd::nds {

struct RegisterUse {
  std::uint64_t reads = 0, writes = 0;
  std::uint32_t last_written = 0;
  unsigned size = 0;  // Width of the most recent access, in bytes.
};

class Arm9Io : public Io {
 public:
  Arm9Io();

  std::uint32_t read(std::uint32_t address, unsigned size) override;
  void write(std::uint32_t address, unsigned size, std::uint32_t value) override;

  // Sets a register without counting it as an access by the program.
  void set(std::uint32_t address, unsigned size, std::uint32_t value);

  // Accesses to registers that have no model, by address.
  const std::map<std::uint32_t, RegisterUse>& unmodelled() const { return unmodelled_; }

 private:
  static constexpr std::uint32_t kStoredSize = 0x2000;  // Registers live in the first 8 KiB.
  std::uint32_t load(std::uint32_t address, unsigned size) const;
  void store(std::uint32_t address, unsigned size, std::uint32_t value);

  std::vector<std::uint8_t> stored_;
  std::map<std::uint32_t, RegisterUse> unmodelled_;
};

}  // namespace bd::nds
