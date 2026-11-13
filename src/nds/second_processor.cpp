#include "nds/second_processor.h"

namespace bd::nds {

SecondProcessor::SecondProcessor(Arm9Bus& bus) : bus_(bus) {}

void SecondProcessor::connect(Ipc& ipc) {
  ipc_ = &ipc;
  // The handshake: the second processor shows 7, and each time the main
  // program echoes the value it sees, shows one less, down to 0. (As NooDS
  // does when it stands in for the second processor.)
  ipc_->peer_set_sync(7);
}

void SecondProcessor::sync_written(unsigned value) {
  if (handshake_done_ || ipc_ == nullptr) return;
  if (value > 0) {
    ipc_->peer_set_sync(value - 1);
    return;
  }
  // The main program echoed the final 0. Its services wait for the second
  // processor's to be ready; mark them so. (The value NooDS sets: service
  // numbers 4 to 17.)
  bus_.write32(kServicesReady, 0x0003FFF0);
  handshake_done_ = true;
}

void SecondProcessor::interrupt_requested() { ++interrupts_requested_; }

void SecondProcessor::word_sent(std::uint32_t word) {
  ++words_by_service_[word & 0x1Fu];
  if (first_words_.size() < kFirstWordsKept) first_words_.push_back(word);
}

}  // namespace bd::nds
