// A stand-in for the sound program the second processor runs.
//
// A game's main program does not touch the sound hardware. It writes lists of
// commands into main memory and sends the second processor the address of
// each list (service 7); the second processor's sound program carries the
// commands out, and counts the lists it has finished in a block of memory the
// two share, where it also keeps the main program informed of what is
// playing.
//
// That program is not run here. This class does what it does, as read out of
// the second-processor program of Golden Sun: Dark Dawn (the routines that
// receive the words, work through the lists, drive the hardware and run the
// alarms), on the sound hardware in sound_hardware.h.
//
// What is here:
// - Receiving: a word whose data is an address in main memory queues a
//   command list (up to eight wait); a word whose data is 0 asks for the
//   waiting lists to be worked through now.
// - The program's rhythm: every 2,728 timer ticks (5.2 ms) it works through
//   the waiting lists and brings the shared block up to date.
// - The commands that drive the hardware directly: setting up, starting and
//   stopping channels and capture units, volume, place, timer, the surround
//   effect, the master volume and output choice, and reserving channels.
// - Alarms: eight timers that send the main program a word when they fire,
//   once or at regular intervals.
// - The shared block: the count of finished lists, which channels and
//   capture units are active, and the variables the main program sets.
//
// - Sequences: music and most sound effects, which the program plays itself
//   from sequence data the main program points it to, on channels it hands
//   out as notes begin (sequence_player.h). Each beat the sequences advance
//   after the lists are worked through, and the shared block shows which
//   players are playing and how many ticks each has played.
//
// What is not: the command that reads the program's internal state. It is
// counted as not carried out; a list is still counted as finished, as the
// main program would otherwise wait for ever.
//
// Timing: a list is worked through, and an alarm's word sent, at the instant
// it is due; on a console the second processor takes a little time. The
// program's rhythm is taken to start with the console; on a console it starts
// when the second processor's program gets to it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "nds/arm9_bus.h"
#include "nds/sequence_player.h"
#include "nds/sound_hardware.h"
#include "nds/timeline.h"

namespace bd::nds {

class SoundDriver : public Timeline::Source {
 public:
  using Time = Timeline::Time;

  static constexpr unsigned kService = 7;
  // The second processor's programs count time in ticks of 64 system clock
  // cycles.
  static constexpr Time kCyclesPerTick = 64;
  // The program's rhythm, and how long after its start the first beat comes.
  static constexpr Time kPeriod = 0xAA8 * kCyclesPerTick;
  static constexpr Time kFirstBeat = 0x10000 * kCyclesPerTick;
  static constexpr std::size_t kListsWaiting = 8;
  static constexpr unsigned kAlarms = 8;
  static constexpr unsigned kCommands = 0x22;

  // A command in a list: six 32-bit words.
  static constexpr std::uint32_t kCommandNext = 0x00;  // Address of the next command, or 0.
  static constexpr std::uint32_t kCommandNumber = 0x04;
  static constexpr std::uint32_t kCommandArguments = 0x08;  // Four.
  static constexpr std::uint32_t kCommandSize = 0x18;

  // The commands.
  enum Command : std::uint32_t {
    kStartSequence = 0x00,
    kStopSequence = 0x01,
    kPrepareSequence = 0x02,
    kStartPreparedSequence = 0x03,
    kPauseSequence = 0x04,
    kSkipSequence = 0x05,
    kSetPlayerValue = 0x06,
    kSetTrackValue = 0x07,
    kMuteTracks = 0x08,
    kSetPlayerChannels = 0x09,
    kSetPlayerVariable = 0x0A,   // Player, variable, value.
    kSetGlobalVariable = 0x0B,   // Variable, value.
    kStart = 0x0C,               // Channels, capture units and alarms, each a set of bits.
    kStop = 0x0D,                // The same, and whether stopped channels hold their level.
    kSetUpSampled = 0x0E,
    kSetUpSquareWave = 0x0F,
    kSetUpNoise = 0x10,
    kSetUpCapture = 0x11,
    kSetUpAlarm = 0x12,          // Alarm, ticks until it fires, ticks between firings (0: once), identifier.
    kSetTimer = 0x13,            // Channels, timer.
    kSetVolume = 0x14,           // Channels, volume, divider.
    kSetPlace = 0x15,            // Channels, place between left and right.
    kSetSurround = 0x16,
    kSetMasterVolume = 0x17,
    kSetPlaceForAll = 0x18,      // A place for every channel, or a negative number for each its own.
    kChooseOutputs = 0x19,       // Left from, right from, channel 1 out of the mixer, channel 3 out.
    kReserveChannels = 0x1A,     // Channels, and which of the two sets of reserved channels.
    kReleaseChannels = 0x1B,
    kStopUnreserved = 0x1C,      // Channels.
    kSetSharedBlock = 0x1D,      // Address.
    kForgetSequenceData = 0x1E,
    kForgetBankData = 0x1F,
    kForgetWaveData = 0x20,
    kReadState = 0x21,
  };

  // The block of main memory the main program shares, by offset.
  static constexpr std::uint32_t kSharedListsFinished = 0x00;   // 32 bits.
  static constexpr std::uint32_t kSharedPlayersPlaying = 0x04;  // 32 bits, one per player.
  static constexpr std::uint32_t kSharedChannelsActive = 0x08;  // 16 bits, one per channel.
  static constexpr std::uint32_t kSharedCapturesActive = 0x0A;  // 16 bits, one per unit.
  static constexpr std::uint32_t kSharedPlayerVariables = 0x20; // 16 of 16 bits each per player, then
  static constexpr std::uint32_t kSharedPlayerTicks = 0x40;     // 32 bits: the ticks it has played;
  static constexpr std::uint32_t kSharedPlayerSize = 0x24;      // players this far apart.
  static constexpr std::uint32_t kSharedGlobalVariables = 0x260;

  // The channels the surround effect applies to: all but 1 and 3.
  static constexpr std::uint32_t kSurroundChannels = 0xFFF5;

  struct Statistics {
    std::uint64_t lists_received = 0;
    std::uint64_t lists_dropped = 0;    // Came while eight were waiting.
    std::uint64_t lists_finished = 0;
    std::uint64_t wake_ups = 0;         // Words asking for the lists to be worked through at once.
    std::uint64_t beats = 0;
    std::uint64_t alarms_fired = 0;
    std::uint64_t alarm_words_sent = 0;
    std::array<std::uint64_t, kCommands> commands{};  // By number.
  };

  // All three must outlive this object.
  SoundDriver(Timeline& timeline, Arm9Bus& bus, SoundHardware& hardware);

  // Sends the main program a word for the sound service with this data.
  // Returns false if the queue to the main program is full. Must be set
  // before an alarm can fire.
  std::function<bool(std::uint32_t data)> send;
  // Called when the driver has changed main memory: address, bytes.
  std::function<void(std::uint32_t, std::size_t)> on_memory_written;

  // What the program does to the hardware as it starts: sound on, outputs
  // from the mixer, master volume full.
  void start_up();

  // The data of a word the main program sent to the sound service.
  void word(std::uint32_t data);

  Time next_event() const override;
  void run_until(Time now) override;

  std::uint32_t shared_block() const { return shared_; }
  const Statistics& statistics() const { return statistics_; }
  const SequencePlayer& sequences() const { return sequences_; }
  // Commands and situations that could not be dealt with as the program
  // would, in words, with how often each came.
  const std::map<std::string, std::uint64_t>& not_carried_out() const { return not_carried_out_; }

  // What a command is, in a few words, for reports.
  static const char* command_name(std::uint32_t number);

 private:
  friend class SequencePlayer;

  struct Alarm {
    bool set = false;             // The program's own flag: started and not stopped since.
    std::uint8_t identifier = 0;
    std::uint8_t unsent = 0;      // Firings whose word is still to be sent.
    std::uint64_t ticks = 0, between = 0;
    Time due = Timeline::kNever;
    Time every = 0;               // 0: once.
    std::uint32_t data = 0;       // What its word carries, fixed when it is started.
  };

  // Works through the waiting lists, as the program does on every beat and
  // when woken.
  void work(Time now);
  void command(std::uint32_t number, const std::uint32_t* a, Time now);
  void update_shared();
  void fire(Alarm& alarm);
  void start_alarm(unsigned n, Time now);
  void stop_alarm(unsigned n);

  // The hardware, as the program's routines use it.
  std::uint32_t reg(std::uint32_t address, unsigned size) const { return hardware_.read(address, size); }
  void set_reg(std::uint32_t address, unsigned size, std::uint32_t value) { hardware_.write(address, size, value); }
  static std::uint32_t channel_reg(unsigned channel, std::uint32_t offset) {
    return SoundHardware::kFirstRegister + 16 * channel + offset;
  }
  void stop_channel(unsigned channel, std::uint32_t hold);
  std::uint32_t surround(std::uint32_t volume, std::uint32_t place) const;
  bool surround_applies(unsigned channel) const {
    return surround_decay_ > 0 && ((kSurroundChannels >> channel) & 1u) != 0;
  }
  void stop_unreserved(std::uint32_t channels);

  // Main memory, as the second processor sees it. Reads and writes outside
  // it are noted and come to nothing.
  std::uint32_t memory(std::uint32_t address);
  void set_memory(std::uint32_t address, unsigned size, std::uint32_t value);

  Timeline& timeline_;
  Arm9Bus& bus_;
  SoundHardware& hardware_;
  std::deque<std::uint32_t> lists_;
  Time next_beat_ = kFirstBeat;
  std::array<Alarm, kAlarms> alarms_{};
  std::uint32_t shared_ = 0;
  // The program's own record of what was asked for, from which it works out
  // what to put in the registers.
  std::int32_t place_for_all_ = -1;
  std::int32_t surround_decay_ = 0;
  std::array<std::uint8_t, SoundHardware::kChannels> asked_volume_{}, asked_place_{};
  std::uint32_t reserved_[2] = {0, 0};  // [1]: the set chosen by bit 0 of the command's second argument.
  Statistics statistics_;
  std::map<std::string, std::uint64_t> not_carried_out_;
  SequencePlayer sequences_{*this};
};

}  // namespace bd::nds
