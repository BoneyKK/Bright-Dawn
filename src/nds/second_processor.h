// A stand-in for the DS's second processor.
//
// On a real console the second processor runs its own program, which looks
// after sound, the touch screen, the clock, power, wireless and the
// cartridge's save memory, and talks to the main program over the channel in
// ipc.h. That program is not run here. This class answers the main program's
// side of the conversation directly instead.
//
// So far it does one thing: the handshake at start-up. Everything else the
// main program sends is recorded and not answered.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/ipc.h"

namespace bd::nds {

class SecondProcessor : public IpcPeer {
 public:
  // The word in main memory where the second processor marks which of its
  // services are ready, one bit per service number.
  static constexpr std::uint32_t kServicesReady = 0x027FFF8C;

  explicit SecondProcessor(Arm9Bus& bus);

  // Must be called once, with the channel this is the other end of, before
  // the program starts.
  void connect(Ipc& ipc);

  void sync_written(unsigned value) override;
  void interrupt_requested() override;
  void word_sent(std::uint32_t word) override;

  bool handshake_done() const { return handshake_done_; }

  // Words the main program sent, counted by service number (the low five
  // bits of each word), and the first few as they came.
  const std::map<unsigned, std::uint64_t>& words_by_service() const { return words_by_service_; }
  const std::vector<std::uint32_t>& first_words() const { return first_words_; }
  std::uint64_t interrupts_requested() const { return interrupts_requested_; }

 private:
  static constexpr std::size_t kFirstWordsKept = 64;
  Arm9Bus& bus_;
  Ipc* ipc_ = nullptr;
  bool handshake_done_ = false;
  std::map<unsigned, std::uint64_t> words_by_service_;
  std::vector<std::uint32_t> first_words_;
  std::uint64_t interrupts_requested_ = 0;
};

}  // namespace bd::nds
