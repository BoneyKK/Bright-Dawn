#include "nds/cartridge.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

// Slot control register.
constexpr u32 kSlotWritable = 0xE043;
constexpr u32 kTransferInterrupt = 1u << 14;  // Request an interrupt when a block has been read.

// Transfer control register.
constexpr u32 kWordReady = 1u << 23;      // Read only.
constexpr unsigned kBlockSizeShift = 24;  // 0: no data. 1-6: 0x100 << n bytes. 7: four bytes.
constexpr u32 kReleaseReset = 1u << 29;   // Can be set, never cleared.
constexpr u32 kBusy = 1u << 31;           // Written as one to start; reads one until the block is done.
constexpr u32 kControlWritable = 0xDF7F7FFF;

// "Read data" cannot reach the first 32 KiB of a cartridge: an address in it
// reads from the 512 bytes after it instead.
constexpr u32 kProtectedEnd = 0x8000;

}  // namespace

void Cartridge::attach(Arm9Io& io) {
  io.add(0x040001A0, 2, [this] { return slot_control_; },
         [this](u32 v, u32 m) {
           m &= kSlotWritable;
           slot_control_ = (slot_control_ & ~m) | (v & m);
         });
  io.add(0x040001A4, 4, [this] { return control_ | (word_ready_ ? kWordReady : 0); },
         [this](u32 v, u32 m) { write_control(v, m); });
  for (int half = 0; half < 2; ++half) {
    io.add(0x040001A8 + 4 * static_cast<u32>(half), 4,
           [this, half] {
             u32 value = 0;
             for (int i = 0; i < 4; ++i) value |= static_cast<u32>(command_[half * 4 + i]) << (8 * i);
             return value;
           },
           [this, half](u32 v, u32 m) {
             for (int i = 0; i < 4; ++i) {
               if ((m >> (8 * i)) & 0xFFu) command_[half * 4 + i] = static_cast<std::uint8_t>(v >> (8 * i));
             }
           });
  }
  io.add(0x04100010, 4, [this] { return read_data(); }, nullptr);
}

void Cartridge::write_control(u32 value, u32 mask) {
  const bool was_busy = (control_ & kBusy) != 0;
  control_ |= value & mask & kReleaseReset;
  mask &= kControlWritable;
  control_ = (control_ & ~mask) | (value & mask);
  if (!was_busy && (control_ & kBusy) != 0) start();
}

void Cartridge::start() {
  const unsigned size = (control_ >> kBlockSizeShift) & 7u;
  const u32 bytes = size == 0 ? 0 : size == 7 ? 4 : (0x100u << size);
  words_left_ = bytes / 4;

  reply_ = Reply::Nothing;
  if (command_[0] == 0xB7) {
    // The address follows, most significant byte first.
    address_ = (static_cast<u32>(command_[1]) << 24) | (static_cast<u32>(command_[2]) << 16) |
               (static_cast<u32>(command_[3]) << 8) | command_[4];
    reply_ = Reply::Data;
    ++statistics_.data_reads;
    if (!inserted()) ++statistics_.reads_without_cartridge;
  } else if (command_[0] == 0xB8) {
    reply_ = Reply::ChipId;
    ++statistics_.chip_id_reads;
  } else {
    ++statistics_.other_commands[command_[0]];
  }

  if (words_left_ == 0) {
    finish();
    return;
  }
  word_ready_ = true;
  pump();
}

u32 Cartridge::next_word() {
  switch (reply_) {
    case Reply::ChipId:
      return kChipId;
    case Reply::Data: {
      u32 address = address_;
      address_ += 4;
      if (!inserted()) return kNoData;
      if (address < kProtectedEnd) address = kProtectedEnd + (address & 0x1FFu);
      if (address > rom_.size() || rom_.size() - address < 4) {
        ++statistics_.reads_beyond_the_end;
        return kNoData;
      }
      statistics_.data_bytes += 4;
      if (address < statistics_.lowest_address) statistics_.lowest_address = address;
      if (address + 3 > statistics_.highest_address) statistics_.highest_address = address + 3;
      return static_cast<u32>(rom_[address]) | (static_cast<u32>(rom_[address + 1]) << 8) |
             (static_cast<u32>(rom_[address + 2]) << 16) | (static_cast<u32>(rom_[address + 3]) << 24);
    }
    case Reply::Nothing:
      break;
  }
  return kNoData;
}

u32 Cartridge::read_data() {
  if (!word_ready_) return 0;  // Nothing is being transferred.
  const u32 word = next_word();
  if (--words_left_ == 0) {
    word_ready_ = false;
    finish();
  }
  // Otherwise the next word is ready at once. If a DMA channel is taking the
  // words, the pump that called it carries on; nothing more to do here.
  return word;
}

void Cartridge::finish() {
  control_ &= ~kBusy;
  if (slot_control_ & kTransferInterrupt) interrupts_.request(Interrupts::kCartridgeTransfer);
}

void Cartridge::pump() {
  if (pumping_ || !on_word_ready) return;  // Already inside: the outer call carries on.
  pumping_ = true;
  while (word_ready_ && on_word_ready()) {
  }
  pumping_ = false;
}

}  // namespace bd::nds
