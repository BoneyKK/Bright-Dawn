// The memory the DS main processor sees.
//
// The regions are in the right places and repeat the way the hardware's do.
// Hardware registers are someone else's business (Io). Video memory is nine
// banks the program places itself (video_memory.h). Accesses to addresses
// with nothing behind them are counted, so a run shows what the game needs
// next.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "machine.h"
#include "nds/video_memory.h"

namespace bd::nds {

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
  // Video memory does not count as memory code can be in: no program is
  // known to run code from there, and it has no single place to point at.
  bool has_code_memory(std::uint32_t address) override { return locate(address, false) != nullptr; }

  VideoMemory& video_memory() { return video_; }

  // The two small fast memories are placed and switched on by the program
  // through the system coprocessor. Until then nothing is mapped there.
  //
  // The instruction memory (32 KiB) occupies addresses below `size`, repeating.
  // The data memory (16 KiB) occupies `size` bytes from `base`, repeating.
  // Either can be readable and writable separately: while a program is filling
  // one, writes go to it and reads go to whatever else is at that address.
  // Where both claim an address the instruction memory wins, and both win over
  // everything else.
  void map_itcm(bool readable, bool writable, std::uint64_t size);
  void map_dtcm(bool readable, bool writable, std::uint32_t base, std::uint64_t size);

  // Puts read-only code at the top of the address space, where the
  // processor's exception vectors are (0xFFFF0000, at most 4 KiB). Writes
  // there are counted as going nowhere.
  void set_bios(const std::uint8_t* data, std::size_t size);

  // Copies bytes into main memory, as the console's loader does before the
  // program starts. Returns false if the range is not inside main memory.
  bool load(std::uint32_t address, const std::uint8_t* data, std::size_t size);

  // Main memory as the second processor sees it: the same 4 MiB, repeating
  // from 0x02000000 to 0x02FFFFFF, with neither fast memory in front of it.
  // `size` is 1, 2 or 4 and `address` is aligned to it.
  static bool in_main_memory(std::uint32_t address) { return (address >> 24) == 0x02; }
  // `address` must be in main memory.
  std::uint32_t main_read(std::uint32_t address, unsigned size) const;
  void main_write(std::uint32_t address, unsigned size, std::uint32_t value);

  const BusStatistics& statistics() const { return statistics_; }

 private:
  struct FastMemory {
    bool readable = false, writable = false;
    std::uint32_t base = 0;
    std::uint64_t size = 0;
    bool claims(std::uint32_t address, bool write) const {
      return (write ? writable : readable) && static_cast<std::uint64_t>(address - base) < size &&
             address >= base;
    }
  };
  // Where `address` lives for a read or a write, or nullptr.
  std::uint8_t* locate(std::uint32_t address, bool write);
  bool is_io(std::uint32_t address, bool write) const;
  bool is_video(std::uint32_t address, bool write) const;
  std::uint32_t read(std::uint32_t address, unsigned size);
  void write(std::uint32_t address, unsigned size, std::uint32_t value);

  Io& io_;
  std::vector<std::uint8_t> main_ram_, itcm_, dtcm_, shared_ram_, palette_, sprites_, bios_;
  VideoMemory video_;
  FastMemory itcm_map_, dtcm_map_;
  BusStatistics statistics_;
};

}  // namespace bd::nds
