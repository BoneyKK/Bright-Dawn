#include "nds/second_processor.h"

namespace bd::nds {

SecondProcessor::SecondProcessor(Arm9Bus& bus, std::uint32_t chip_id) : bus_(bus), chip_id_(chip_id) {}

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
      default:
        // The requests that work on the save memory.
        ++not_carried_out_[name + ", which is not implemented"];
        return;
    }
  }
  send(word(kCartridgeService, true, 1));
}

}  // namespace bd::nds
