// The cartridge slot as the main processor uses it once a game is running:
// a command of eight bytes goes out, and a block of data comes back a word
// at a time.
//
// Only the state after start-up is modelled, in which a cartridge accepts two
// commands: read data and read the chip identifier. The encrypted exchange
// the firmware performs before that is not needed, because the game is
// started directly. The cartridge's save memory, which is reached through
// separate registers, is not modelled.
//
// Words are ready at once: a transfer takes no console time.
#pragma once

#include <cstdint>
#include <functional>
#include <map>

#include "core/bytes.h"
#include "nds/arm9_io.h"
#include "nds/interrupts.h"

namespace bd::nds {

class Cartridge {
 public:
  // What an empty slot, or a read beyond the cartridge's data, returns.
  static constexpr std::uint32_t kNoData = 0xFFFFFFFF;
  static constexpr std::uint32_t kChipId = 0x00001FC2;

  explicit Cartridge(Interrupts& interrupts) : interrupts_(interrupts) {}

  // The cartridge's contents: a ROM image, which must outlive this object.
  // Without one the slot is empty.
  void insert(ByteSpan rom) { rom_ = rom; }
  bool inserted() const { return !rom_.empty(); }

  // Registers: slot control (0x040001A0), transfer control (0x040001A4), the
  // command (0x040001A8, eight bytes) and data in (0x04100010).
  void attach(Arm9Io& io);

  // Called when a word has become ready to be read. DMA channels that are
  // waiting for the cartridge move it; returns whether one did.
  std::function<bool()> on_word_ready;
  // Offers the word that is ready, if any, to on_word_ready, and goes on
  // doing so for as long as words are taken.
  void pump();
  bool word_ready() const { return word_ready_; }

  struct Statistics {
    std::uint64_t data_reads = 0;        // "Read data" commands.
    std::uint64_t data_bytes = 0;        // Bytes they returned.
    std::uint32_t lowest_address = ~std::uint32_t{0}, highest_address = 0;  // Of the data read.
    std::uint64_t chip_id_reads = 0;
    std::uint64_t reads_without_cartridge = 0;
    std::uint64_t reads_beyond_the_end = 0;  // Words.
    // Commands that are neither, by first byte.
    std::map<unsigned, std::uint64_t> other_commands;
  };
  const Statistics& statistics() const { return statistics_; }

 private:
  enum class Reply { Nothing, Data, ChipId };
  void write_control(std::uint32_t value, std::uint32_t mask);
  void start();
  std::uint32_t read_data();
  std::uint32_t next_word();
  void finish();

  Interrupts& interrupts_;
  ByteSpan rom_;
  std::uint32_t slot_control_ = 0, control_ = 0;
  std::uint8_t command_[8] = {};
  Reply reply_ = Reply::Nothing;
  std::uint32_t address_ = 0;          // Of the next word, for Reply::Data.
  std::uint32_t words_left_ = 0;
  bool word_ready_ = false;
  bool pumping_ = false;
  Statistics statistics_;
};

}  // namespace bd::nds
