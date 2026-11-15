// The stand-in for the second processor's sound program: command lists,
// alarms and the block of memory it shares with the main program.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/ipc.h"
#include "nds/save_memory.h"
#include "nds/second_processor.h"
#include "nds/sound_driver.h"
#include "nds/sound_hardware.h"
#include "nds/timeline.h"
#include "test.h"

namespace {

using bd::nds::SoundDriver;
using bd::nds::SoundHardware;
using Time = bd::nds::Timeline::Time;

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

constexpr std::uint32_t kShared = 0x021081C0;  // Where a program might keep the shared block,
constexpr std::uint32_t kLists = 0x02108440;   // its commands,
constexpr std::uint32_t kBuffer = 0x02084A00;  // and a buffer for sound.
constexpr Time kTick = SoundDriver::kCyclesPerTick;

struct Command {
  std::uint32_t number;
  std::array<std::uint32_t, 4> arguments;
};

std::uint32_t channel(unsigned n, std::uint32_t reg = 0) { return SoundHardware::kFirstRegister + 16 * n + reg; }

struct Fixture {
  NoIo no_io;
  bd::nds::Arm9Bus bus{no_io};
  bd::nds::Timeline timeline;
  SoundHardware hardware{bus};
  SoundDriver driver{timeline, bus, hardware};
  std::vector<std::uint32_t> sent;
  std::vector<std::pair<std::uint32_t, std::size_t>> written;
  bool queue_full = false;
  std::uint32_t next_list = kLists;

  Fixture() {
    timeline.add(hardware);
    driver.send = [this](std::uint32_t data) {
      if (queue_full) return false;
      sent.push_back(data);
      return true;
    };
    driver.on_memory_written = [this](std::uint32_t a, std::size_t n) { written.emplace_back(a, n); };
    driver.start_up();
  }
  // Writes a list of commands into memory, as the main program does, and
  // returns its address.
  std::uint32_t list(std::initializer_list<Command> commands) {
    const std::uint32_t first = next_list;
    std::size_t left = commands.size();
    for (const Command& c : commands) {
      --left;
      bus.write32(next_list + 0, left != 0 ? next_list + 0x18 : 0);
      bus.write32(next_list + 4, c.number);
      for (unsigned i = 0; i < 4; ++i) bus.write32(next_list + 8 + 4 * i, c.arguments[i]);
      next_list += 0x18;
    }
    return first;
  }
  // Sends a list and has it worked through at once.
  void run(std::initializer_list<Command> commands) {
    driver.word(list(commands));
    driver.word(0);
  }
  void share() { run({{SoundDriver::kSetSharedBlock, {kShared, 0, 0, 0}}}); }
  std::uint32_t finished() { return bus.read32(kShared); }
  std::uint32_t control(unsigned n) { return hardware.read(channel(n), 4); }
};

// The commands with which Golden Sun: Dark Dawn sets up its echo (seen in a
// run of the real program): channels 1 and 3 play, for ever, the two buffers
// the capture units record the mixer into, and an alarm tells the main
// program each time half a buffer has gone by.
std::initializer_list<Command> echo_commands() {
  static const std::initializer_list<Command> commands = {
      {SoundDriver::kReserveChannels, {0x0000000A, 0, 0, 0}},
      {SoundDriver::kSetUpSampled, {0x02000001, 0x02084A00, 0x7F000154, 0x05000000}},
      {SoundDriver::kSetUpCapture, {0x02084A00, 0x00000154, 0x20000000, 0}},
      {SoundDriver::kSetUpSampled, {0x02000003, 0x02084F50, 0x7F000154, 0x057F0000}},
      {SoundDriver::kSetUpCapture, {0x02084F50, 0x00000154, 0xA0000000, 0}},
      {SoundDriver::kSetUpAlarm, {0, 0x1640, 0x1540, 1}},
      {SoundDriver::kStart, {0x0000000A, 3, 1, 0}},
      {SoundDriver::kChooseOutputs, {1, 2, 1, 1}},
  };
  return commands;
}

}  // namespace

TEST(sound_driver_turns_the_sound_on_as_it_starts) {
  Fixture f;
  // Enabled, both outputs from the mixer, master volume full.
  CHECK(f.hardware.read(SoundHardware::kControl, 2) == 0x807F);
  CHECK(f.driver.shared_block() == 0);
}

TEST(sound_driver_works_through_lists_on_its_beat) {
  Fixture f;
  f.driver.word(f.list({{SoundDriver::kSetSharedBlock, {kShared, 0, 0, 0}}}));
  CHECK(f.driver.next_event() == SoundDriver::kFirstBeat);
  f.timeline.advance_to(SoundDriver::kFirstBeat - 1);
  CHECK(f.driver.shared_block() == 0);
  CHECK(f.driver.statistics().lists_received == 1 && f.driver.statistics().lists_finished == 0);
  f.timeline.advance_to(SoundDriver::kFirstBeat);
  CHECK(f.driver.shared_block() == kShared);
  // The list that sets the block is the first to be counted in it.
  CHECK(f.finished() == 1);
  CHECK(f.driver.statistics().lists_finished == 1 && f.driver.statistics().beats == 1);
  CHECK(f.driver.next_event() == SoundDriver::kFirstBeat + SoundDriver::kPeriod);

  // Two lists before the next beat are both done on it, in order.
  f.driver.word(f.list({{SoundDriver::kSetMasterVolume, {0x40, 0, 0, 0}}}));
  f.driver.word(f.list({{SoundDriver::kSetMasterVolume, {0x20, 0, 0, 0}}}));
  f.timeline.advance_to(SoundDriver::kFirstBeat + SoundDriver::kPeriod - 1);
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x7F);
  f.timeline.advance_to(SoundDriver::kFirstBeat + SoundDriver::kPeriod);
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x20);
  CHECK(f.finished() == 3);
  CHECK(f.driver.statistics().commands[SoundDriver::kSetMasterVolume] == 2);
  CHECK(f.driver.not_carried_out().empty());
}

TEST(sound_driver_works_through_lists_at_once_when_woken) {
  Fixture f;
  f.share();
  CHECK(f.finished() == 1);
  f.driver.word(f.list({{SoundDriver::kSetMasterVolume, {0x11, 0, 0, 0}}, {SoundDriver::kSetMasterVolume, {0x22, 0, 0, 0}}}));
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x7F);
  f.driver.word(0);
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x22);
  CHECK(f.finished() == 2);
  CHECK(f.driver.statistics().wake_ups == 2);
  // Being woken with nothing waiting changes nothing; nor does a word that is
  // neither an address in main memory nor zero.
  f.driver.word(0);
  f.driver.word(0x01FFFFFF);
  f.driver.word(0);
  CHECK(f.finished() == 2);
  // The count's four bytes were reported as written.
  bool reported = false;
  for (const auto& w : f.written) reported = reported || w == std::make_pair(kShared, std::size_t{4});
  CHECK(reported);
}

TEST(sound_driver_lets_eight_lists_wait) {
  Fixture f;
  f.share();
  for (unsigned i = 0; i < 9; ++i) f.driver.word(f.list({{SoundDriver::kSetMasterVolume, {i + 1, 0, 0, 0}}}));
  CHECK(f.driver.statistics().lists_dropped == 1);
  f.driver.word(0);
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 8);  // The ninth never happened.
  CHECK(f.finished() == 9);
}

TEST(sound_driver_counts_a_list_whatever_is_in_it) {
  Fixture f;
  f.share();
  f.run({{SoundDriver::kStartSequence, {0, 0, 0, 0}},
         {0x22, {0, 0, 0, 0}},  // No such command: skipped.
         {SoundDriver::kReadState, {0x02200000, 0, 0, 0}},
         {SoundDriver::kStartSequence, {1, 0, 0, 0}},
         {SoundDriver::kSetMasterVolume, {0x33, 0, 0, 0}}});
  CHECK(f.finished() == 2);
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x33);
  CHECK(f.driver.not_carried_out().size() == 2);
  CHECK(f.driver.not_carried_out().at("\"start a sequence\", which is not implemented") == 2);
  CHECK(f.driver.not_carried_out().at("\"read the sound program's state\", which is not implemented") == 1);
  CHECK(f.driver.statistics().commands[SoundDriver::kStartSequence] == 2);
}

TEST(sound_driver_notes_a_list_finished_with_no_shared_block) {
  Fixture f;
  f.run({{SoundDriver::kSetMasterVolume, {0x33, 0, 0, 0}}, {SoundDriver::kSetGlobalVariable, {0, 5, 0, 0}}});
  CHECK(f.hardware.read(SoundHardware::kControl, 1) == 0x33);
  CHECK(f.driver.statistics().lists_finished == 1);
  CHECK(f.driver.not_carried_out().at("counting a finished list before the shared block was set") == 1);
  CHECK(f.driver.not_carried_out().at("setting a variable before the shared block was set") == 1);
  CHECK(f.written.empty());
}

TEST(sound_driver_sets_up_the_game_s_echo) {
  Fixture f;
  f.share();
  f.driver.word(f.list(echo_commands()));
  f.timeline.advance_to(SoundDriver::kFirstBeat);
  const Time started = SoundDriver::kFirstBeat;

  // Channels 1 and 3: started, 16-bit, looping, full volume, hard left and
  // hard right.
  CHECK(f.control(1) == 0xA800007F);
  CHECK(f.control(3) == 0xA87F007F);
  CHECK(f.control(0) == 0 && f.control(2) == 0);
  // Capture units: started, 16-bit, looping, from the mixer.
  CHECK(f.hardware.read(SoundHardware::kCaptureControl, 2) == 0x8080);
  CHECK(f.hardware.read(SoundHardware::kCaptureDestination, 4) == 0x02084A00);
  CHECK(f.hardware.read(SoundHardware::kCaptureDestination + 8, 4) == 0x02084F50);
  // Left output from channel 1, right from channel 3, both kept out of the
  // mixer; still enabled, master volume untouched.
  CHECK(f.hardware.read(SoundHardware::kControl, 2) == 0xB97F);
  // What the main program is told.
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0x000A);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedCapturesActive) == 0x0003);
  CHECK(f.finished() == 2);
  CHECK(f.driver.not_carried_out().empty());

  // The timer asked for is 0x200 ticks of the sound clock per value: one
  // 16-bit value per output sample from each unit.
  f.timeline.advance_to(started + 100 * SoundHardware::kCyclesPerSample);
  CHECK(f.hardware.statistics().bytes_captured == 2 * 2 * 100);
  CHECK(f.hardware.notes().empty());

  // The alarm: first after 0x1640 ticks, then every 0x1540, carrying its
  // number (0) and the identifier it was given (1).
  CHECK(f.sent.empty());
  f.timeline.advance_to(started + 0x1640 * kTick - 1);
  CHECK(f.sent.empty());
  f.timeline.advance_to(started + 0x1640 * kTick);
  CHECK(f.sent.size() == 1 && f.sent[0] == 0x100);
  f.timeline.advance_to(started + (0x1640 + 0x1540) * kTick - 1);
  CHECK(f.sent.size() == 1);
  f.timeline.advance_to(started + (0x1640 + 3 * 0x1540) * kTick);
  CHECK(f.sent.size() == 4 && f.sent[3] == 0x100);
  CHECK(f.driver.statistics().alarms_fired == 4 && f.driver.statistics().alarm_words_sent == 4);
}

TEST(sound_driver_alarm_fires_once_when_it_has_no_interval) {
  Fixture f;
  f.run({{SoundDriver::kSetUpAlarm, {5, 100, 0, 0x42}}, {SoundDriver::kStart, {0, 0, 1u << 5, 0}}});
  CHECK(f.driver.next_event() == 100 * kTick);
  f.timeline.advance_to(100 * kTick);
  CHECK(f.sent.size() == 1 && f.sent[0] == 0x4205);
  f.timeline.advance_to(SoundDriver::kFirstBeat * 2);
  CHECK(f.sent.size() == 1);
}

TEST(sound_driver_stopped_alarm_is_silent_and_comes_back_with_a_new_identifier) {
  Fixture f;
  f.run({{SoundDriver::kSetUpAlarm, {2, 10, 10, 7}}, {SoundDriver::kStart, {0, 0, 4, 0}}});
  f.timeline.advance_to(25 * kTick);
  CHECK(f.sent.size() == 2 && f.sent[1] == 0x0702);
  f.run({{SoundDriver::kStop, {0, 0, 4, 0}}});
  f.timeline.advance_to(1000 * kTick);
  CHECK(f.sent.size() == 2);
  // Stopping moved the identifier on by one.
  f.run({{SoundDriver::kStart, {0, 0, 4, 0}}});
  f.timeline.advance_to(1010 * kTick);
  CHECK(f.sent.size() == 3 && f.sent[2] == 0x0802);
  // Setting it up again stops it and gives it the identifier asked for.
  f.run({{SoundDriver::kSetUpAlarm, {2, 10, 10, 0x30}}});
  f.timeline.advance_to(2000 * kTick);
  CHECK(f.sent.size() == 3);
  f.run({{SoundDriver::kStart, {0, 0, 4, 0}}});
  f.timeline.advance_to(2010 * kTick);
  CHECK(f.sent.size() == 4 && f.sent[3] == 0x3002);
  // Stopping an alarm that is not running changes nothing.
  f.run({{SoundDriver::kStop, {0, 0, 0xFA, 0}}});
  f.run({{SoundDriver::kSetUpAlarm, {8, 10, 10, 0}}});
  CHECK(f.driver.not_carried_out().at("setting up an alarm past the eighth") == 1);
}

TEST(sound_driver_alarm_words_wait_for_room_in_the_queue) {
  Fixture f;
  f.run({{SoundDriver::kSetUpAlarm, {0, 10, 10, 1}}, {SoundDriver::kStart, {0, 0, 1, 0}}});
  f.queue_full = true;
  f.timeline.advance_to(35 * kTick);  // Three firings, nothing sent.
  CHECK(f.sent.empty());
  CHECK(f.driver.statistics().alarms_fired == 3 && f.driver.statistics().alarm_words_sent == 0);
  f.queue_full = false;
  f.timeline.advance_to(39 * kTick);  // Room alone sends nothing: the next firing does.
  CHECK(f.sent.empty());
  f.timeline.advance_to(40 * kTick);
  CHECK(f.sent.size() == 4);
  f.timeline.advance_to(50 * kTick);
  CHECK(f.sent.size() == 5);
}

TEST(sound_driver_sets_channels_up_for_each_kind_of_sound) {
  Fixture f;
  // Sampled: channel 4, timer 0x123, data at kBuffer, 8-bit compressed,
  // played once, loop start 0x10, length 0x200, volume 0x55 divided by 4,
  // place 0x30.
  f.run({{SoundDriver::kSetUpSampled, {0x01230004, kBuffer, (0x55u << 24) | (2u << 22) | 0x200, (2u << 26) | (2u << 24) | (0x30u << 16) | 0x10}}});
  CHECK(f.control(4) == ((2u << 29) | (2u << 27) | (0x30u << 16) | (2u << 8) | 0x55));
  // Square wave: channel 9, duty 3, volume 0x40 halved, timer 0x456, place 0x7F.
  f.run({{SoundDriver::kSetUpSquareWave, {9, (1u << 8) | 0x40, (0x456u << 8) | 0x7F, 3}}});
  CHECK(f.control(9) == (0x60000000u | (3u << 24) | (0x7Fu << 16) | (1u << 8) | 0x40));
  // Noise: channel 15, volume 0x7F, timer 0x789, place 0.
  f.run({{SoundDriver::kSetUpNoise, {15, 0x7F, 0x789u << 8, 0}}});
  CHECK(f.control(15) == (0x60000000u | 0x7F));
  // None is started by being set up.
  CHECK(!f.hardware.channel_active(4) && !f.hardware.channel_active(9) && !f.hardware.channel_active(15));
  f.run({{SoundDriver::kStart, {(1u << 4) | (1u << 9) | (1u << 15), 0, 0, 0}}});
  CHECK(f.hardware.channel_active(4) && f.hardware.channel_active(9) && f.hardware.channel_active(15));
  CHECK(f.hardware.statistics().channels_started == 3);

  f.run({{SoundDriver::kSetUpSampled, {0x00010010, kBuffer, 0, 0}}});
  CHECK(f.driver.not_carried_out().at("\"set a channel up for sampled sound\" for a channel past 15") == 1);
}

TEST(sound_driver_square_wave_timer_reaches_the_hardware) {
  // A timer of 0x1000 sound-clock ticks per step is one step every eight
  // output samples: listen to the wave change through a capture unit.
  Fixture f;
  f.run({{SoundDriver::kSetUpSquareWave, {8, 0x7F, 0x1000u << 8, 3}},
         {SoundDriver::kSetUpCapture, {0x02200000, 32, 0x20000000, 0}},
         {SoundDriver::kSetTimer, {1u << 1, 0x200, 0, 0}},  // The capture unit goes by channel 1's timer.
         {SoundDriver::kStart, {1u << 8, 1, 0, 0}}});
  f.timeline.advance_to(40 * SoundHardware::kCyclesPerSample);
  // Duty 3: low for four steps, high for four. 32 samples low, then high.
  CHECK(static_cast<std::int16_t>(f.bus.read16(0x02200000 + 2 * 31)) == -0x7FFF);
  CHECK(static_cast<std::int16_t>(f.bus.read16(0x02200000 + 2 * 32)) == 0x7FFF);
}

TEST(sound_driver_changes_volume_place_and_timer_of_sets_of_channels) {
  Fixture f;
  f.run({{SoundDriver::kSetVolume, {0x0005, 0x33, 2, 0}}});
  CHECK(f.control(0) == 0x0233 && f.control(2) == 0x0233 && f.control(1) == 0);
  f.run({{SoundDriver::kSetPlace, {0x0006, 0x44, 0, 0}}});
  CHECK(f.control(1) == 0x00440000 && f.control(2) == 0x00440233 && f.control(0) == 0x0233);

  // One place for all channels overrides each channel's own, until a
  // negative number gives them back.
  f.run({{SoundDriver::kSetPlaceForAll, {0x10, 0, 0, 0}}});
  CHECK(f.control(1) == 0x00100000 && f.control(2) == 0x00100233 && f.control(15) == 0x00100000);
  f.run({{SoundDriver::kSetPlace, {0x0001, 0x55, 0, 0}}});
  CHECK(f.control(0) == 0x00100233);  // Remembered, not applied.
  f.run({{SoundDriver::kSetUpNoise, {14, 0x7F, 0x100u << 8 | 0x22, 0}}});
  CHECK(f.control(14) == (0x60000000u | (0x10u << 16) | 0x7F));
  f.run({{SoundDriver::kSetPlaceForAll, {0xFFFFFFFF, 0, 0, 0}}});
  CHECK(f.control(0) == 0x00550233 && f.control(1) == 0x00440000 && f.control(2) == 0x00440233);
  CHECK(f.control(14) == (0x60000000u | (0x22u << 16) | 0x7F) && f.control(15) == 0);
}

TEST(sound_driver_surround_makes_sound_at_the_sides_quieter) {
  // By hand, for a strength of 0x4000 and volume 100: at place 0 the factor
  // is 0x4000 * 40 + (0x7FFF - 0x4000) * 64 = 1,703,872, and 100 times that,
  // shifted right 21, is 81. At place 127 it is -0x4000 * 87 + (0x4000 +
  // 0x7FFF) * 64 = 1,720,256, which gives 82. Between 24 and 104 nothing
  // changes.
  Fixture f;
  f.run({{SoundDriver::kSetVolume, {0x0017, 100, 0, 0}},
         {SoundDriver::kSetPlace, {0x0001, 0, 0, 0}},
         {SoundDriver::kSetPlace, {0x0006, 127, 0, 0}},
         {SoundDriver::kSetPlace, {0x0010, 64, 0, 0}},
         {SoundDriver::kSetSurround, {0x4000, 0, 0, 0}}});
  CHECK((f.control(0) & 0x7F) == 81);
  CHECK((f.control(2) & 0x7F) == 82);
  CHECK((f.control(4) & 0x7F) == 100);
  CHECK((f.control(1) & 0x7F) == 100);  // Channels 1 and 3 are left alone.
  // With the effect on, new volumes and places go through it.
  f.run({{SoundDriver::kSetVolume, {0x0001, 50, 0, 0}}});
  CHECK((f.control(0) & 0x7F) == 40);  // 50 * 1,703,872 >> 21.
  f.run({{SoundDriver::kSetPlace, {0x0001, 64, 0, 0}}});
  CHECK((f.control(0) & 0x7F) == 50);
  f.run({{SoundDriver::kSetUpNoise, {14, 100, 127, 0}}});
  CHECK((f.control(14) & 0x7F) == 82);
  // Off again: the volumes asked for come back, less one at the sides,
  // because the program still puts them through its arithmetic, in which
  // "no change" is 32,767 parts in 32,768.
  f.run({{SoundDriver::kSetSurround, {0, 0, 0, 0}}});
  CHECK((f.control(2) & 0x7F) == 99 && (f.control(14) & 0x7F) == 99);
  CHECK((f.control(0) & 0x7F) == 50 && (f.control(4) & 0x7F) == 100);
  // A volume set from now on is left alone.
  f.run({{SoundDriver::kSetVolume, {0x0004, 100, 0, 0}}});
  CHECK((f.control(2) & 0x7F) == 100);
}

TEST(sound_driver_stops_channels_and_capture_units) {
  Fixture f;
  f.share();
  f.run(echo_commands());
  f.run({{SoundDriver::kStop, {0x0002, 1, 0, 1}}});  // Channel 1, holding its level, and unit 0.
  CHECK(f.control(1) == 0x2800807F);
  CHECK(f.hardware.channel_active(3));
  CHECK(f.hardware.read(SoundHardware::kCaptureControl, 2) == 0x8000);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0x0008);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedCapturesActive) == 0x0002);
  f.run({{SoundDriver::kStop, {0x0008, 2, 0, 0}}});
  CHECK(f.control(3) == 0x287F007F);
  CHECK(f.hardware.read(SoundHardware::kCaptureControl, 2) == 0);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedCapturesActive) == 0);
  // Starting one capture unit by itself.
  f.run({{SoundDriver::kStart, {0, 2, 0, 0}}});
  CHECK(f.hardware.read(SoundHardware::kCaptureControl, 2) == 0x8000);
  f.run({{SoundDriver::kStart, {0, 1, 0, 0}}});
  CHECK(f.hardware.read(SoundHardware::kCaptureControl, 2) == 0x8080);
}

TEST(sound_driver_tells_the_main_program_when_a_channel_has_played_out) {
  Fixture f;
  f.share();
  // 8-bit, one word, played once, one byte per output sample.
  f.run({{SoundDriver::kSetUpSampled, {0x02000005, kBuffer, 0x7F000001, 2u << 26}}, {SoundDriver::kStart, {1u << 5, 0, 0, 0}}});
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0x0020);
  // Four samples later the channel is done; the block says so from the next
  // beat on.
  f.timeline.advance_to(SoundDriver::kFirstBeat - 1);
  CHECK(!f.hardware.channel_active(5));
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0x0020);
  f.timeline.advance_to(SoundDriver::kFirstBeat);
  CHECK(f.bus.read16(kShared + SoundDriver::kSharedChannelsActive) == 0);
}

TEST(sound_driver_reserved_channels_are_not_stopped_with_the_rest) {
  Fixture f;
  const auto start_all = [&] {
    f.run({{SoundDriver::kSetUpNoise, {14, 0x7F, 0x100u << 8, 0}},
           {SoundDriver::kSetUpNoise, {15, 0x7F, 0x100u << 8, 0}},
           {SoundDriver::kStart, {0xC000, 0, 0, 0}}});
  };
  start_all();
  // Reserving a channel stops it first.
  f.run({{SoundDriver::kReserveChannels, {0x4000, 0, 0, 0}}});
  CHECK(!f.hardware.channel_active(14) && f.hardware.channel_active(15));
  start_all();
  f.run({{SoundDriver::kStopUnreserved, {0xC000, 0, 0, 0}}});
  CHECK(f.hardware.channel_active(14) && !f.hardware.channel_active(15));
  // The second set of reserved channels does not protect a channel.
  f.run({{SoundDriver::kReleaseChannels, {0x4000, 0, 0, 0}}, {SoundDriver::kReserveChannels, {0x4000, 1, 0, 0}}});
  CHECK(!f.hardware.channel_active(14));
  start_all();
  f.run({{SoundDriver::kStopUnreserved, {0x4000, 0, 0, 0}}});
  CHECK(!f.hardware.channel_active(14) && f.hardware.channel_active(15));
}

TEST(sound_driver_keeps_the_main_program_s_variables_in_the_shared_block) {
  Fixture f;
  f.share();
  f.run({{SoundDriver::kSetPlayerVariable, {3, 5, 0x1234, 0}},
         {SoundDriver::kSetPlayerVariable, {0, 0, 0xFFFFFFFF, 0}},
         {SoundDriver::kSetGlobalVariable, {15, 0xABCD, 0, 0}}});
  CHECK(f.bus.read16(kShared + 0x20 + 3 * 0x24 + 5 * 2) == 0x1234);
  CHECK(f.bus.read16(kShared + 0x20) == 0xFFFF);
  CHECK(f.bus.read16(kShared + 0x260 + 15 * 2) == 0xABCD);
  CHECK(f.driver.not_carried_out().empty());
}

TEST(sound_driver_survives_lists_that_lead_nowhere) {
  Fixture f;
  f.share();
  // A list whose second command is outside main memory.
  const std::uint32_t at = f.list({{SoundDriver::kSetMasterVolume, {0x12, 0, 0, 0}}});
  f.bus.write32(at, 0x03000000);
  f.driver.word(at);
  // A list that goes round in a circle.
  const std::uint32_t circle = f.list({{SoundDriver::kSetMasterVolume, {0x13, 0, 0, 0}}});
  f.bus.write32(circle, circle);
  f.driver.word(circle);
  f.driver.word(0);
  CHECK(f.finished() == 3);
  CHECK(f.driver.not_carried_out().size() == 2);
  CHECK(f.driver.statistics().commands[SoundDriver::kSetMasterVolume] == 1 + 65536);
}

TEST(second_processor_passes_sound_words_both_ways) {
  NoIo no_io;
  bd::nds::Arm9Bus bus{no_io};
  bd::nds::Arm9Io io;
  bd::nds::Interrupts irq;
  bd::nds::SaveMemory save;
  bd::nds::Timeline timeline;
  SoundHardware hardware{bus};
  SoundDriver driver{timeline, bus, hardware};
  bd::nds::SecondProcessor second{bus, 0x00001FC2, save};
  bd::nds::Ipc ipc{irq, second};
  std::vector<std::pair<std::uint32_t, std::size_t>> written;
  ipc.attach(io);
  second.connect(ipc);
  second.connect_sound(driver);
  second.on_memory_written = [&](std::uint32_t a, std::size_t n) { written.emplace_back(a, n); };
  driver.start_up();
  io.write(0x04000184, 2, 0x8000);  // The program turns the queues on.

  // A list that sets the shared block and an alarm, then a wake-up word. The
  // flag on a word makes no difference to this service.
  const std::uint32_t list[] = {kLists + 0x18, SoundDriver::kSetSharedBlock, kShared, 0, 0, 0,
                                kLists + 0x30, SoundDriver::kSetUpAlarm, 3, 10, 10, 9,
                                0, SoundDriver::kStart, 0, 0, 8, 0};
  for (unsigned i = 0; i < 18; ++i) bus.write32(kLists + 4 * i, list[i]);
  io.write(0x04000188, 4, bd::nds::SecondProcessor::word(7, true, kLists));
  io.write(0x04000188, 4, bd::nds::SecondProcessor::word(7, false, 0));
  CHECK(bus.read32(kShared) == 1);
  CHECK(!written.empty());
  CHECK(second.unanswered_by_service().empty());
  CHECK(second.words_by_service().at(7) == 2);

  // The alarm's words arrive as words from service 7, and stop coming when
  // the queue's sixteen places are taken.
  timeline.advance_to(20 * kTick);
  CHECK(second.words_answered() == 2);
  CHECK(io.read(0x04100000, 4) == bd::nds::SecondProcessor::word(7, false, 0x0903));
  timeline.advance_to(300 * kTick);
  CHECK(second.words_answered() == 17);
  CHECK(ipc.waiting_words() == 16);
  for (int i = 0; i < 16; ++i) CHECK(io.read(0x04100000, 4) == bd::nds::SecondProcessor::word(7, false, 0x0903));
  // The firings that found no room are made up for at the next one.
  // Thirteen found none; with the next one that is fourteen words.
  CHECK(ipc.waiting_words() == 0);
  timeline.advance_to(310 * kTick);
  CHECK(ipc.waiting_words() == 14);
}
