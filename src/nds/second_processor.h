// A stand-in for the DS's second processor.
//
// On a real console the second processor runs its own program, which looks
// after sound, the touch screen, the clock, power, wireless and the
// cartridge's save memory, and talks to the main program over the channel in
// ipc.h. That program is not run here. This class answers the main program's
// side of the conversation directly instead.
//
// What it does was read out of the second-processor program of Golden Sun:
// Dark Dawn, the routines named below, unless said otherwise.
//
// - It leaves the console's settings in memory the way that program does as
//   it starts (user_settings.h).
// - It performs the handshake at start-up and marks the services as ready.
// - It receives words as that program's receiving routine does. A word is a
//   service number (bits 0-4), a flag (bit 5) and 26 bits of data. Words for
//   service 0 are dropped. A word for a service the program has no handler
//   for is sent back with its flag set, or dropped if the flag was set
//   already.
// - Service 11, requests to the cartridge: "initialise" and "read the chip
//   identifier" are carried out, and the three requests the program refuses
//   are refused the same way. The others, which work on the save memory, are
//   recorded and not answered.
//
// Words for the other services the program handles are recorded and not
// answered. An answer is sent the moment the request arrives; on a console
// it takes the second processor some time.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/ipc.h"
#include "nds/user_settings.h"

namespace bd::nds {

class SecondProcessor : public IpcPeer {
 public:
  // The word in main memory where the second processor marks which of its
  // services are ready, one bit per service number.
  static constexpr std::uint32_t kServicesReady = 0x027FFF8C;
  // The services the game's second-processor program registers a handler
  // for: numbers 4 to 14, 16 and 17.
  static constexpr std::uint32_t kServices = 0x00037FF0;

  static constexpr unsigned kCartridgeService = 11;
  // Requests to the cartridge service that are carried out here.
  static constexpr std::uint32_t kCartridgeInitialise = 0;
  static constexpr std::uint32_t kCartridgeReadChipId = 3;
  // The block of memory the main program shares for cartridge requests: the
  // result of the last request, a value returned by some, and the set of
  // requests the main program allows, one bit per request number.
  static constexpr std::uint32_t kCartridgeBlockResult = 0x00;
  static constexpr std::uint32_t kCartridgeBlockValue = 0x08;
  static constexpr std::uint32_t kCartridgeBlockAllowed = 0x58;
  static constexpr std::uint32_t kCartridgeDone = 0;     // Results.
  static constexpr std::uint32_t kCartridgeRefused = 3;

  // A word as both programs put it on the channel.
  static constexpr std::uint32_t word(unsigned service, bool flag, std::uint32_t data) {
    return (service & 0x1Fu) | (flag ? 0x20u : 0u) | (data << 6);
  }

  // `chip_id` is what the cartridge answers when asked what it is.
  SecondProcessor(Arm9Bus& bus, std::uint32_t chip_id);

  // Must be called once, with the channel this is the other end of, before
  // the program starts.
  void connect(Ipc& ipc);

  // What the second-processor program does with the console's settings when
  // it starts, before the main program can look: the settings block, the
  // network address and the wireless channels go to their places in main
  // memory. Fails, writing nothing, if the settings are not valid.
  Status start_up(const UserSettings& settings);

  // Called when the stand-in has changed main memory: the address and the
  // number of bytes.
  std::function<void(std::uint32_t, std::size_t)> on_memory_written;

  void sync_written(unsigned value) override;
  void interrupt_requested() override;
  void word_sent(std::uint32_t word) override;

  bool handshake_done() const { return handshake_done_; }

  // Words the main program sent, counted by service number, and the first
  // few as they came.
  const std::map<unsigned, std::uint64_t>& words_by_service() const { return words_by_service_; }
  const std::vector<std::uint32_t>& first_words() const { return first_words_; }
  // Of those, the words for services that have no stand-in yet.
  const std::map<unsigned, std::uint64_t>& unanswered_by_service() const { return unanswered_by_service_; }
  // Requests to a service that has a stand-in which could not be carried
  // out, in words, with how often each came. Nothing was answered to these.
  const std::map<std::string, std::uint64_t>& not_carried_out() const { return not_carried_out_; }
  // Words sent to the main program.
  std::uint64_t words_answered() const { return words_answered_; }
  std::uint64_t interrupts_requested() const { return interrupts_requested_; }

 private:
  static constexpr std::size_t kFirstWordsKept = 64;
  // "No block yet, and the next word is a request number."
  static constexpr std::uint32_t kNoCartridgeBlock = 0x80000000;

  void send(std::uint32_t word);
  void write(std::uint32_t address, unsigned size, std::uint32_t value);
  void cartridge_word(std::uint32_t data);
  void cartridge_request();

  Arm9Bus& bus_;
  std::uint32_t chip_id_;
  Ipc* ipc_ = nullptr;
  bool handshake_done_ = false;
  // The cartridge service's two words of state, as the program keeps them.
  std::uint32_t cartridge_block_ = kNoCartridgeBlock;
  std::uint32_t cartridge_request_ = 0;
  std::map<unsigned, std::uint64_t> words_by_service_;
  std::map<unsigned, std::uint64_t> unanswered_by_service_;
  std::map<std::string, std::uint64_t> not_carried_out_;
  std::vector<std::uint32_t> first_words_;
  std::uint64_t words_answered_ = 0;
  std::uint64_t interrupts_requested_ = 0;
};

}  // namespace bd::nds
