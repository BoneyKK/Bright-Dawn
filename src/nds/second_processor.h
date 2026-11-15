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
// - Service 11, requests to the cartridge: "initialise", "read the chip
//   identifier" and the requests on the save memory that this game's kind of
//   chip takes (save_memory.h) are carried out, and the three requests the
//   program refuses are refused the same way. Anything else is recorded and
//   not answered.
// - It passes on the two buttons and the lid sensor that are wired to it
//   (report_buttons).
// - Service 6, the touch screen: a request for one sample is answered with
//   the state set through set_touch, and the request that sets how far apart
//   readings may lie is accepted. Sampling at regular intervals is not
//   implemented.
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
#include "nds/save_memory.h"
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

  // A word as both programs put it on the channel.
  static constexpr std::uint32_t word(unsigned service, bool flag, std::uint32_t data) {
    return (service & 0x1Fu) | (flag ? 0x20u : 0u) | (data << 6);
  }

  static constexpr unsigned kTouchService = 6;
  // Requests to the touch screen service.
  static constexpr unsigned kTouchSampleOnce = 0;
  static constexpr unsigned kTouchSampleRegularly = 1;
  static constexpr unsigned kTouchStopSampling = 2;
  static constexpr unsigned kTouchSetTolerance = 3;
  // Results of a touch screen request, in the answer's low byte.
  static constexpr unsigned kTouchDone = 0;
  static constexpr unsigned kTouchUnknownRequest = 1;
  static constexpr unsigned kTouchBadArgument = 2;
  static constexpr unsigned kTouchWrongState = 3;
  // Where a sample is left for the main program: 32 bits, as two 16-bit
  // halves. Bits 0-11 the horizontal reading, 12-23 the vertical, bit 24 set
  // while the screen is touched, bits 25-26 set for a reading that is not
  // usable (both when the screen is not touched).
  static constexpr std::uint32_t kTouchSample = 0x027FFFAA;
  // A request to this group of services is built from 16-bit pieces, one per
  // word: the piece in the data's low 16 bits, its position in bits 16-19,
  // bit 25 on the first word and bit 24 on the last. The request's number is
  // the high byte of piece 0. The answer has both bits, the request's number
  // with its top bit set in bits 8-15, and the result in the low byte. It
  // goes out on the service the request's number belongs to, whichever
  // service the request came in on: 0x00-0x1F touch screen (6), 0x20-0x3F
  // firmware storage (4), 0x40-0x5F microphone (9), 0x60-0x7F power (8).
  static constexpr std::uint32_t kPieceFirst = 0x02000000;
  static constexpr std::uint32_t kPieceLast = 0x01000000;
  static constexpr std::uint32_t group_answer(unsigned request, unsigned result) {
    const unsigned group = (request >> 5) & 3u;
    const unsigned service = group == 0 ? 6u : group == 1 ? 4u : group == 2 ? 9u : 8u;
    return word(service, false, kPieceFirst | kPieceLast | (((request & 0xFFu) | 0x80u) << 8) | (result & 0xFFu));
  }

  static constexpr unsigned kCartridgeService = 11;
  // Requests to the cartridge service that are carried out here.
  static constexpr std::uint32_t kCartridgeInitialise = 0;
  static constexpr std::uint32_t kCartridgePrepareSave = 2;  // Sets the chip's status register, if asked to.
  static constexpr std::uint32_t kCartridgeReadChipId = 3;
  static constexpr std::uint32_t kCartridgeReadSave = 6;
  static constexpr std::uint32_t kCartridgeWriteSave = 7;    // Replaces bytes.
  static constexpr std::uint32_t kCartridgeProgramSave = 8;  // Clears bits.
  static constexpr std::uint32_t kCartridgeVerifySave = 9;   // Compares the chip with memory.
  static constexpr std::uint32_t kCartridgeEraseSectors = 11;
  static constexpr std::uint32_t kCartridgeReadStatus = 13;
  // The block of memory the main program shares for cartridge requests.
  static constexpr std::uint32_t kCartridgeBlockResult = 0x00;      // Of the last request.
  static constexpr std::uint32_t kCartridgeBlockSaveKind = 0x04;    // What save memory the game says it has.
  static constexpr std::uint32_t kCartridgeBlockValue = 0x08;       // Returned by "read the chip identifier".
  static constexpr std::uint32_t kCartridgeBlockSource = 0x0C;      // Where to copy from,
  static constexpr std::uint32_t kCartridgeBlockDestination = 0x10; // where to,
  static constexpr std::uint32_t kCartridgeBlockLength = 0x14;      // and how many bytes.
  static constexpr std::uint32_t kCartridgeBlockSaveSize = 0x18;
  static constexpr std::uint32_t kCartridgeBlockSectorSize = 0x1C;
  static constexpr std::uint32_t kCartridgeBlockPageSize = 0x24;
  static constexpr std::uint32_t kCartridgeBlockAddressBytes = 0x28;
  static constexpr std::uint32_t kCartridgeBlockStatusWanted = 0x54;  // One byte; 0xFF: leave it.
  static constexpr std::uint32_t kCartridgeBlockAllowed = 0x58;     // Requests allowed, one bit each.
  // Results.
  static constexpr std::uint32_t kCartridgeDone = 0;
  static constexpr std::uint32_t kCartridgeDiffers = 1;       // "Verify" found a difference.
  static constexpr std::uint32_t kCartridgeBadArgument = 2;   // An erase not on sector boundaries.
  static constexpr std::uint32_t kCartridgeRefused = 3;
  // How this game describes its save memory in the block: serial flash,
  // 2 to the 18th bytes.
  static constexpr std::uint32_t kSaveKind = 0x1202;

  // `chip_id` is what the cartridge answers when asked what it is; `save` is
  // the cartridge's save memory. Both must outlive this object.
  SecondProcessor(Arm9Bus& bus, std::uint32_t chip_id, SaveMemory& save);

  // Must be called once, with the channel this is the other end of, before
  // the program starts.
  void connect(Ipc& ipc);

  // What the second-processor program does with the console's settings when
  // it starts, before the main program can look: the settings block, the
  // network address and the wireless channels go to their places in main
  // memory. Fails, writing nothing, if the settings are not valid.
  Status start_up(const UserSettings& settings);

  // Where the second-processor program leaves the inputs wired to it, for
  // the main program: X in bit 10 and Y in bit 11, 0 = held, as the main
  // program's own button register has its ten; bit 13 for a button only
  // development consoles have; bit 15 set while the lid is closed.
  static constexpr std::uint32_t kButtonWord = 0x027FFFA8;
  // Passes on what the second processor's button register reads
  // (Keypad::second_processor_register). The program does this from a timer,
  // every four milliseconds; here it is to be called at the start and
  // whenever the inputs change.
  void report_buttons(std::uint16_t button_register);

  // The touch screen as the next sample will find it: whether it is being
  // touched, and where, in pixels of the touch screen (0-255 across, 0-191
  // down). The readings handed to the main program are the ones a console
  // with the calibration in the settings block (user_settings.h) would give
  // for that pixel, with no noise.
  void set_touch(bool touched, unsigned x, unsigned y);

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
  void touch_word(std::uint32_t data);
  void cartridge_word(std::uint32_t data);
  void cartridge_request();
  // A request on the save memory. Returns false, with the reason in *why,
  // if it cannot be carried out; otherwise the result is in the block.
  bool save_request(std::uint32_t block, std::string* why);

  Arm9Bus& bus_;
  std::uint32_t chip_id_;
  SaveMemory& save_;
  bool save_status_set_ = false;
  Ipc* ipc_ = nullptr;
  bool handshake_done_ = false;
  // The touch screen service: the pieces of the request being received.
  std::uint16_t touch_pieces_[16] = {};
  bool touched_ = false;
  unsigned touch_x_ = 0, touch_y_ = 0;
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
