// The hardware registers of the DS main processor.
//
// Parts of the hardware register the registers they implement here. A
// register nobody implements keeps the value last written to it, and every
// access to one is counted, so that a run of the game's code shows which
// registers it still needs and in what order of importance.
#pragma once

#include <cstdint>
#include <functional>
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
  // Reads return the whole register. Writes receive the new value and a mask
  // of the bits being written: a program may write one byte of a register.
  using Read = std::function<std::uint32_t()>;
  using Write = std::function<void(std::uint32_t value, std::uint32_t mask)>;

  Arm9Io();

  // Implements the register of `width` bytes (1, 2 or 4) at `address`. A null
  // `read` reads as zero; a null `write` ignores writes. Registers must not
  // overlap.
  void add(std::uint32_t address, unsigned width, Read read, Write write);

  std::uint32_t read(std::uint32_t address, unsigned size) override;
  void write(std::uint32_t address, unsigned size, std::uint32_t value) override;

  // Sets an unimplemented register without counting it as an access by the
  // program.
  void set(std::uint32_t address, unsigned size, std::uint32_t value);

  // Accesses that touched a register nobody implements, by address.
  const std::map<std::uint32_t, RegisterUse>& unmodelled() const { return unmodelled_; }

 private:
  struct Register {
    unsigned width = 0;
    Read read;
    Write write;
  };
  static constexpr std::uint32_t kStoredSize = 0x2000;  // Registers live in the first 8 KiB.
  // The register containing `address`, with its own address in *base.
  const Register* find(std::uint32_t address, std::uint32_t* base) const;
  std::uint8_t load(std::uint32_t address) const;
  void store(std::uint32_t address, std::uint8_t value);

  std::map<std::uint32_t, Register> registers_;
  std::vector<std::uint8_t> stored_;
  std::map<std::uint32_t, RegisterUse> unmodelled_;
};

}  // namespace bd::nds
