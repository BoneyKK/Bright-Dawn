#include "nds/second_processor.h"

#include <algorithm>

namespace bd::nds {

namespace {

// Whether `size` bytes from `address` all lie in main memory.
bool in_main_memory(std::uint32_t address, std::uint32_t size) {
  return Arm9Bus::in_main_memory(address) && size <= 0x03000000u - address;
}

}  // namespace

SecondProcessor::SecondProcessor(Arm9Bus& bus, std::uint32_t chip_id, SaveMemory& save)
    : bus_(bus), chip_id_(chip_id), save_(save) {}

void SecondProcessor::connect(Ipc& ipc) {
  ipc_ = &ipc;
  // The handshake: the second processor shows 7, and each time the main
  // program echoes the value it sees, shows one less, down to 0. (As NooDS
  // does when it stands in for the second processor.)
  ipc_->peer_set_sync(7);
}

void SecondProcessor::write(std::uint32_t address, unsigned size, std::uint32_t value) {
  bus_.main_write(address, size, value);
  if (on_memory_written) on_memory_written(address, size);
}

void SecondProcessor::send(std::uint32_t word) {
  if (ipc_ == nullptr) return;
  ++words_answered_;
  ipc_->peer_send(word);
}

Status SecondProcessor::start_up(const UserSettings& settings) {
  const auto block = user_settings_block(settings);
  if (!block) return block.error();
  for (std::size_t i = 0; i < block.value().size(); ++i)
    write(kUserSettingsAddress + static_cast<std::uint32_t>(i), 1, block.value()[i]);
  for (std::size_t i = 0; i < settings.network_address.size(); ++i)
    write(kNetworkAddressAddress + static_cast<std::uint32_t>(i), 1, settings.network_address[i]);
  write(kWirelessChannelsAddress, 2, chosen_wireless_channels(settings.wireless_channels));
  return {};
}

void SecondProcessor::sync_written(unsigned value) {
  if (handshake_done_ || ipc_ == nullptr) return;
  if (value > 0) {
    ipc_->peer_set_sync(value - 1);
    return;
  }
  // The main program echoed the final 0. Its services wait for the second
  // processor's to be ready; mark them so. On a console each service's bit
  // appears as the second-processor program gets to it; here all are there
  // at once.
  write(kServicesReady, 4, kServices);
  handshake_done_ = true;
}

void SecondProcessor::interrupt_requested() { ++interrupts_requested_; }

void SecondProcessor::word_sent(std::uint32_t word) {
  const unsigned service = word & 0x1Fu;
  const bool flag = (word & 0x20u) != 0;
  const std::uint32_t data = word >> 6;
  ++words_by_service_[service];
  if (first_words_.size() < kFirstWordsKept) first_words_.push_back(word);

  if (service == 0) return;
  if (service == kTouchService) {
    // Its handler takes no notice of words with the flag.
    if (!flag) touch_word(data);
    return;
  }
  if (service == kCartridgeService) {
    // Its handler takes no notice of words without the flag.
    if (flag) cartridge_word(data);
    return;
  }
  if ((kServices >> service) & 1u) {
    ++unanswered_by_service_[service];
    return;
  }
  // No handler: the word goes back marked, unless it came marked.
  if (!flag) send(word | 0x20u);
}

void SecondProcessor::set_touch(bool touched, unsigned x, unsigned y) {
  touched_ = touched;
  touch_x_ = x > 255 ? 255 : x;
  touch_y_ = y > 191 ? 191 : y;
}

// The touch screen service's handler, and what its thread then does.
void SecondProcessor::touch_word(std::uint32_t data) {
  if (data & kPieceFirst) {
    for (std::uint16_t& piece : touch_pieces_) piece = 0;
  }
  touch_pieces_[(data >> 16) & 0xFu] = static_cast<std::uint16_t>(data);
  if ((data & kPieceLast) == 0) return;

  const unsigned request = touch_pieces_[0] >> 8;
  const unsigned argument = touch_pieces_[0] & 0xFFu;
  switch (request) {
    case kTouchSampleOnce: {
      // A touched screen is read five times in each direction and the
      // readings combined; with no noise that gives the reading itself, less
      // its low three bits, which the program clears.
      // (The top five bits of a sample are whatever the program's stack
      // held; here they are zero.)
      std::uint32_t sample = 0x06000000;  // Not touched: no reading, neither direction usable.
      if (touched_) {
        const std::uint32_t x = (touch_x_ * kTouchReadingPerPixel) & 0xFF8u;
        const std::uint32_t y = (touch_y_ * kTouchReadingPerPixel) & 0xFF8u;
        sample = x | (y << 12) | 0x01000000;
      }
      write(kTouchSample, 2, sample & 0xFFFFu);
      write(kTouchSample + 2, 2, sample >> 16);
      send(group_answer(request, kTouchDone));
      break;
    }
    case kTouchSampleRegularly: {
      // Samples per frame (1 to 4) and the display line to start on.
      if (argument == 0 || argument > 4 || touch_pieces_[1] >= 263) {
        send(group_answer(request, kTouchBadArgument));
        break;
      }
      ++not_carried_out_["service 6: sampling at regular intervals, which is not implemented"];
      break;
    }
    case kTouchStopSampling:
      // Regular sampling is never running here.
      send(group_answer(request, kTouchWrongState));
      break;
    case kTouchSetTolerance:
      // How far apart readings may lie and still count as one. Without noise
      // it makes no difference; it is accepted as the program accepts it.
      send(group_answer(request, argument == 0 ? kTouchBadArgument : kTouchDone));
      break;
    default:
      send(group_answer(request, kTouchUnknownRequest));
      break;
  }
}

// The cartridge service's handler. A request is one word, its number. The
// first request, "initialise", is followed by a second word: the address of
// the block the two programs share from then on.
void SecondProcessor::cartridge_word(std::uint32_t data) {
  if (cartridge_block_ != 0) cartridge_request_ = data;
  if (cartridge_request_ == kCartridgeInitialise) {
    if (cartridge_block_ == kNoCartridgeBlock) {
      cartridge_block_ = 0;  // The block's address comes next.
      return;
    }
    cartridge_block_ = data;
  } else if (cartridge_request_ == 1 || cartridge_request_ > 15) {
    // The second-processor program stops itself on these.
    ++not_carried_out_["service 11: request " + std::to_string(cartridge_request_) +
                       ", on which the second processor's program stops"];
    return;
  }
  cartridge_request();
}

// The task the handler starts: carry the request out, leave the result in the
// shared block, tell the main program.
void SecondProcessor::cartridge_request() {
  const std::uint32_t block = cartridge_block_;
  const std::string name = "service 11: request " + std::to_string(cartridge_request_);
  if (!Arm9Bus::in_main_memory(block) || (block & 3u) != 0) {
    ++not_carried_out_[name + ", with a shared block that is not in main memory"];
    return;
  }
  const std::uint32_t allowed = bus_.main_read(block + kCartridgeBlockAllowed, 4);
  if (((allowed >> cartridge_request_) & 1u) == 0) {
    write(block + kCartridgeBlockResult, 4, kCartridgeRefused);
  } else {
    switch (cartridge_request_) {
      case kCartridgeInitialise:
        write(block + kCartridgeBlockResult, 4, kCartridgeDone);
        break;
      case kCartridgeReadChipId:
        write(block + kCartridgeBlockResult, 4, kCartridgeDone);
        write(block + kCartridgeBlockValue, 4, chip_id_);
        break;
      case 4:
      case 5:
      case 10:
        write(block + kCartridgeBlockResult, 4, kCartridgeRefused);
        break;
      default: {
        std::string why;
        if (!save_request(block, &why)) {
          ++not_carried_out_[name + ", " + why];
          return;
        }
        break;
      }
    }
  }
  send(word(kCartridgeService, true, 1));
}

// The requests on the save memory, each as the second-processor program's
// routine for it uses the chip. Those routines also wait for the chip to
// finish and give up after a time the block names; this chip is never busy.
bool SecondProcessor::save_request(std::uint32_t block, std::string* why) {
  const auto field = [&](std::uint32_t offset) { return bus_.main_read(block + offset, 4); };
  if (field(kCartridgeBlockSaveKind) != kSaveKind || field(kCartridgeBlockSaveSize) != SaveMemory::kSize ||
      field(kCartridgeBlockAddressBytes) != 3) {
    *why = "for a kind of save memory other than this game's";
    return false;
  }
  const std::uint32_t source = field(kCartridgeBlockSource);
  const std::uint32_t destination = field(kCartridgeBlockDestination);
  const std::uint32_t length = field(kCartridgeBlockLength);
  std::uint32_t result = kCartridgeDone;

  switch (cartridge_request_) {
    case kCartridgePrepareSave: {
      const std::uint32_t wanted = bus_.main_read(block + kCartridgeBlockStatusWanted, 1);
      if (wanted == 0xFF || save_status_set_) break;
      if (wanted != save_.status()) {
        *why = "which would change the chip's status register: not implemented";
        return false;
      }
      save_status_set_ = true;
      break;
    }
    case kCartridgeReadSave:  // From the chip at `source` to memory at `destination`.
      if (!in_main_memory(destination, length)) {
        *why = "to memory that is not main memory";
        return false;
      }
      for (std::uint32_t i = 0; i < length; ++i) bus_.main_write(destination + i, 1, save_.read(source + i));
      if (length != 0 && on_memory_written) on_memory_written(destination, length);
      break;
    case kCartridgeWriteSave:    // From memory at `source` to the chip at `destination`,
    case kCartridgeProgramSave: {  // a page at a time.
      const std::uint32_t page = field(kCartridgeBlockPageSize);
      if (page == 0 || (page & (page - 1)) != 0) {
        *why = "with a page size that is not a power of two";
        return false;
      }
      if (!in_main_memory(source, length)) {
        *why = "from memory that is not main memory";
        return false;
      }
      std::vector<std::uint8_t> bytes;
      for (std::uint32_t done = 0; done < length;) {
        const std::uint32_t at = destination + done;
        const std::uint32_t count = std::min(page - (at & (page - 1)), length - done);
        bytes.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
          bytes[i] = static_cast<std::uint8_t>(bus_.main_read(source + done + i, 1));
        if (cartridge_request_ == kCartridgeWriteSave) {
          save_.page_write(at, bytes.data(), count);
        } else {
          save_.page_program(at, bytes.data(), count);
        }
        done += count;
      }
      break;
    }
    case kCartridgeVerifySave:  // The chip at `destination` against memory at `source`.
      if (!in_main_memory(source, length)) {
        *why = "against memory that is not main memory";
        return false;
      }
      for (std::uint32_t i = 0; i < length; ++i) {
        if (save_.read(destination + i) != bus_.main_read(source + i, 1)) {
          result = kCartridgeDiffers;
          break;
        }
      }
      break;
    case kCartridgeEraseSectors: {  // `length` bytes from `destination`, in whole sectors.
      const std::uint32_t sector = field(kCartridgeBlockSectorSize);
      if (((destination | length) & (sector - 1)) != 0) {
        result = kCartridgeBadArgument;
        break;
      }
      // (A sector size of zero with a length has been turned away above.)
      if (length != 0 && length / sector > 1024) {
        *why = "for more sectors than any save memory has";
        return false;
      }
      for (std::uint32_t done = 0; done < length; done += sector) save_.erase_sector(destination + done);
      break;
    }
    case kCartridgeReadStatus:  // One byte, to memory at `destination`.
      if (!in_main_memory(destination, 1)) {
        *why = "to memory that is not main memory";
        return false;
      }
      write(destination, 1, save_.status());
      break;
    default:
      *why = "which is not implemented";
      return false;
  }
  write(block + kCartridgeBlockResult, 4, result);
  return true;
}

}  // namespace bd::nds
