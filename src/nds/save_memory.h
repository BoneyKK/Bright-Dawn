// The memory chip in the cartridge that keeps the player's save.
//
// Golden Sun: Dark Dawn has a serial flash chip of 256 KiB: that is the kind
// and size its main program declares when it first asks the second processor
// about its save memory (seen in a run of the real program). Only the second
// processor talks to the chip; this class is the chip as that program's
// routines use it, by what each of the chip's commands does to its contents.
//
// Flash memory is written in two steps. Erasing sets every bit of a region to
// 1. Programming can only turn 1 bits into 0. The chip also has a command
// that does both for the bytes it is given.
//
// A new chip is erased throughout. Every operation completes at once; on a
// real chip erasing and writing take milliseconds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::nds {

class SaveMemory {
 public:
  static constexpr std::size_t kSize = 256 * 1024;
  static constexpr std::size_t kPageSize = 256;      // The unit of writing.
  static constexpr std::size_t kSectorSize = 65536;  // The unit of erasing.

  struct Statistics {
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;     // By either way of writing.
    std::uint64_t sectors_erased = 0;
  };

  SaveMemory();

  // Addresses are taken modulo the chip's size, as the chip only has that
  // many address lines.
  std::uint8_t read(std::uint32_t address);

  // "Page write": the bytes given replace what was there. "Page program":
  // only bits that are 0 in the bytes given change. Both stay inside the
  // page of the first byte: past the page's end they continue at its start,
  // as the chip does.
  void page_write(std::uint32_t address, const std::uint8_t* data, std::size_t size);
  void page_program(std::uint32_t address, const std::uint8_t* data, std::size_t size);

  // Erases the sector that holds `address`.
  void erase_sector(std::uint32_t address);

  // The chip's status register: bit 0 is set while a write or erase is in
  // progress, which here it never is. Taken to be zero otherwise, as on the
  // flash chips whose commands the game's routines use; which chip is in a
  // given cartridge is not known here.
  std::uint8_t status() const { return 0; }

  // The whole contents, to keep in a file between sessions, and back. `load`
  // fails if the data is not exactly the chip's size.
  ByteSpan contents() const { return ByteSpan(bytes_.data(), bytes_.size()); }
  Status load(ByteSpan data);
  // Whether the contents changed since they were loaded (or since the chip
  // was new).
  bool changed() const { return changed_; }

  const Statistics& statistics() const { return statistics_; }

 private:
  std::vector<std::uint8_t> bytes_;
  bool changed_ = false;
  Statistics statistics_;
};

}  // namespace bd::nds
