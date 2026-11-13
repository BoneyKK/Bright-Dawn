// The memory the DS main processor sees.
//
// This is the first, deliberately plain version: the regions are in the right
// places and mirror the way the hardware does, but the hardware registers are
// only stored, not acted on, and video memory is not banked. Everything that
// is not modelled is counted, so a run shows what the game needs next.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "machine.h"

namespace bd::ds {

// Hardware registers (0x04000000 - 0x04FFFFFF). `size` is 1, 2 or 4 and
// `address` is aligned to it.
class Io {
 public:
  virtual ~Io() = default;
  virtual std::uint32_t read(std::uint32_t address, unsigned size) = 0;
  virtual void write(std::uint32_t address, unsigned size, std::uint32_t value) = 0;
};

struct BusStatistics {
  // Accesses to addresses nothing is mapped at, by 64 KiB block.
  std::map<std::uint32_t, std::uint64_t> unmapped_reads;
  std::map<std::uint32_t, std::uint64_t> unmapped_writes;
};

class Arm9Bus : public runtime::Bus {
 public:
  static constexpr std::uint32_t kMainRamSize = 4 * 1024 * 1024;
  static constexpr std::uint32_t kItcmSize = 32 * 1024;
  static constexpr std::uint32_t kDtcmSize = 16 * 1024;

  explicit Arm9Bus(Io& io);

  std::uint8_t read8(std::uint32_t address) override;
  std::uint16_t read16(std::uint32_t address) override;
  std::uint32_t read32(std::uint32_t address) override;
  void write8(std::uint32_t address, std::uint8_t value) override;
  void write16(std::uint32_t address, std::uint16_t value) override;
  void write32(std::uint32_t address, std::uint32_t value) override;

  // The two small fast memories are placed and switched on by the program
  // through the system coprocessor. Until then nothing is mapped there.
  // The instruction memory always starts at address 0 and repeats up to
  // `itcm_end`; the data memory sits at `dtcm_base` and takes precedence over
  // whatever else is at that address.
  void map_itcm(bool enabled, std::uint32_t end);
  void map_dtcm(bool enabled, std::uint32_t base);
  bool itcm_enabled() const { return itcm_enabled_; }
  bool dtcm_enabled() const { return dtcm_enabled_; }
  std::uint32_t dtcm_base() const { return dtcm_base_; }

  // Copies bytes into main memory, as the console's loader does before the
  // program starts. Returns false if the range is not inside main memory.
  bool load(std::uint32_t address, const std::uint8_t* data, std::size_t size);

  const BusStatistics& statistics() const { return statistics_; }

 private:
  // Where `address` lives, or nullptr. `size` bytes must fit.
  std::uint8_t* locate(std::uint32_t address, unsigned size);
  std::uint32_t read(std::uint32_t address, unsigned size);
  void write(std::uint32_t address, unsigned size, std::uint32_t value);

  Io& io_;
  std::vector<std::uint8_t> main_ram_, itcm_, dtcm_, shared_ram_, palette_, video_, sprites_;
  bool itcm_enabled_ = false, dtcm_enabled_ = false;
  std::uint32_t itcm_end_ = 0, dtcm_base_ = 0;
  BusStatistics statistics_;
};

}  // namespace bd::ds
