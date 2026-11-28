// The sound program's sequence player: its tables, and sequences made up
// here, played through the sound program stand-in on the sound hardware.
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/sequence_player.h"
#include "nds/sound_driver.h"
#include "nds/sound_hardware.h"
#include "nds/timeline.h"
#include "test.h"

namespace {

using bd::nds::SequencePlayer;
using bd::nds::SoundDriver;
using bd::nds::SoundHardware;

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

constexpr std::uint32_t kShared = 0x02100000;
constexpr std::uint32_t kLists = 0x02100400;
constexpr std::uint32_t kWaves = 0x02110000;     // A wave archive with one wave,
constexpr std::uint32_t kBank = 0x02111000;      // a bank whose program 0 plays it,
constexpr std::uint32_t kSequence = 0x02112000;  // and a sequence.

std::uint32_t channel(unsigned n, std::uint32_t reg = 0) { return SoundHardware::kFirstRegister + 16 * n + reg; }

struct Fixture {
  NoIo no_io;
  bd::nds::Arm9Bus bus{no_io};
  bd::nds::Timeline timeline;
  SoundHardware hardware{bus};
  SoundDriver driver{timeline, bus, hardware};
  std::uint32_t next_list = kLists;
  bd::nds::Timeline::Time now = 0;

  Fixture() {
    timeline.add(hardware);
    driver.start_up();
    command(SoundDriver::kSetSharedBlock, {kShared, 0, 0, 0});
    // The wave: 16-bit, looping, a timer of 0x200, 16 words long.
    bus.write32(kWaves + 0x38, 1);
    bus.write32(kWaves + 0x3C, 0x40);
    bus.write32(kWaves + 0x40, 0x00000101);
    bus.write32(kWaves + 0x44, 0x00000200);
    bus.write32(kWaves + 0x48, 16);
    // The bank: linked to the archive, program 0 that wave, played at key 60
    // with the fastest envelope.
    bus.write32(kBank + 0x18, kWaves);
    bus.write32(kBank + 0x38, 2);
    instrument(0, 1, 0, 0);
  }
  void byte(std::uint32_t address, std::uint32_t value) { bus.main_write(address, 1, value); }
  // Program `program` as an instrument of `type`, with these two wave words.
  void instrument(unsigned program, unsigned type, unsigned wave0, unsigned wave1) {
    const std::uint32_t at = kBank + 0x80 + program * 0x10;
    bus.write32(kBank + 0x3C + 4 * program, type | ((at - kBank) << 8));
    bus.write32(at, wave0 | (wave1 << 16));
    const std::uint8_t rest[6] = {60, 127, 127, 127, 127, 64};
    for (unsigned i = 0; i < 6; ++i) byte(at + 4 + i, rest[i]);
  }
  void sequence(std::initializer_list<std::uint8_t> events) {
    std::uint32_t at = kSequence;
    for (std::uint8_t e : events) byte(at++, e);
  }
  void command(std::uint32_t number, std::initializer_list<std::uint32_t> arguments) {
    bus.write32(next_list, 0);
    bus.write32(next_list + 4, number);
    unsigned i = 0;
    for (std::uint32_t a : arguments) bus.write32(next_list + 8 + 4 * i++, a);
    driver.word(next_list);
    driver.word(0);
    next_list += 0x18;
  }
  void play() { command(SoundDriver::kStartSequence, {0, kSequence, 0, kBank}); }
  // Runs the sound program for this many of its beats.
  void beats(unsigned n) {
    if (now == 0) now = SoundDriver::kFirstBeat;
    else now += SoundDriver::kPeriod;
    now += SoundDriver::kPeriod * (n - 1);
    timeline.advance_to(now);
  }
  std::uint32_t playing() { return bus.read32(kShared + SoundDriver::kSharedPlayersPlaying); }
  std::int16_t variable(unsigned n) {
    return static_cast<std::int16_t>(bus.main_read(kShared + SoundDriver::kSharedPlayerVariables + 2 * n, 2));
  }
};

}  // namespace

TEST(sequence_player_tables) {
  CHECK(SequencePlayer::decibels_squared(127) == 0 && SequencePlayer::decibels_squared(64) == -119);
  CHECK(SequencePlayer::decibels_squared(1) == -722 && SequencePlayer::decibels_squared(0) == -32768);
  // Full volume; half the amplitude at 6 dB; a divider below that.
  CHECK(SequencePlayer::channel_volume(0) == 127 && SequencePlayer::channel_volume(-60) == 64);
  CHECK(SequencePlayer::channel_volume(-61) == (126 | 1 << 8) && (SequencePlayer::channel_volume(-723) >> 8) == 3);
  // An octave up halves the timer's period; an octave down doubles it.
  CHECK(SequencePlayer::timer(1000, 0) == 1000 && SequencePlayer::timer(1000, 768) == 500);
  CHECK(SequencePlayer::timer(1000, -768) == 2000 && SequencePlayer::timer(0x8000, -768 * 2) == 0xFFFF);
  CHECK(SequencePlayer::sine(0) == 0 && SequencePlayer::sine(32) == 127 && SequencePlayer::sine(96) == -127);
}

TEST(sequence_player_plays_a_note_and_ends) {
  Fixture f;
  // Key 60 at full velocity for 48 ticks, then the end.
  f.sequence({0x3C, 0x7F, 0x30, 0xFF});
  f.play();
  f.beats(1);
  // The note is on channel 4, the first one handed out, at the wave's own
  // pitch, full volume, in the middle.
  CHECK(f.playing() == 1);
  CHECK(f.hardware.channel_active(4));
  for (unsigned i = 0; i < 16; ++i) f.byte(kWaves + 0x4C + i * 2 + 1, i % 2 == 0 ? 0x40 : 0xC0);
  const std::uint32_t control = f.hardware.read(channel(4), 4);
  CHECK((control & 0x7F) == 127 && ((control >> 16) & 0x7F) == 64);
  CHECK(((control >> SoundHardware::kFormatShift) & 3) == 1 && ((control >> SoundHardware::kRepeatShift) & 3) == 1);
  // 48 ticks at 120 beats a minute: half a second, 96 of the program's beats.
  f.beats(90);
  CHECK(f.playing() == 1 && f.hardware.channel_active(4));
  CHECK(f.hardware.statistics().samples_with_sound > 1000);
  f.beats(10);
  CHECK(f.playing() == 0 && !f.hardware.channel_active(4));
  CHECK(f.bus.read32(kShared + SoundDriver::kSharedPlayerTicks) >= 48);
}

TEST(sequence_player_variables_conditions_and_loops) {
  Fixture f;
  f.sequence({0xB0, 0x00, 0x05, 0x00,        // Variable 0 = 5,
              0xB1, 0x00, 0x02, 0x00,        // + 2;
              0xB8, 0x00, 0x07, 0x00,        // is it 7?
              0xA2, 0xB0, 0x01, 0x09, 0x00,  // If so, variable 1 = 9;
              0xB8, 0x00, 0x08, 0x00,        // is it 8?
              0xA2, 0xB0, 0x03, 0x09, 0x00,  // if so (it is not), variable 3 = 9.
              0xD4, 0x03,                    // Three times:
              0xB1, 0x02, 0x01, 0x00,        //   variable 2 + 1.
              0xFC, 0xFF});
  f.play();
  f.beats(1);
  CHECK(f.variable(0) == 7 && f.variable(1) == 9 && f.variable(3) == -1);
  CHECK(f.variable(2) == 2);  // From -1, as every variable starts.
  CHECK(f.playing() == 0);
}

TEST(sequence_player_plays_square_waves_on_their_channels) {
  Fixture f;
  f.instrument(1, 2, 3, 0);  // Program 1: a square wave, high for half the time.
  f.sequence({0x81, 0x01, 0x45, 0x7F, 0x60, 0xFF});  // Program 1; key 69, 96 ticks.
  f.play();
  f.beats(1);
  CHECK(f.hardware.channel_active(8));
  const std::uint32_t control = f.hardware.read(channel(8), 4);
  CHECK(((control >> SoundHardware::kFormatShift) & 3) == 3 && ((control >> SoundHardware::kDutyShift) & 7) == 3);
  // Nine semitones above the instrument's key: 440 Hz times 2^(9/12), about
  // 740 rises a second; counted over a quarter of a second.
  unsigned rises = 0;
  int last = 0;
  f.hardware.on_output = [&](SoundHardware::Output o) {
    const int now = o.left > 0x200 ? 1 : -1;
    if (now > 0 && last < 0) ++rises;
    last = now;
  };
  f.beats(48);
  CHECK(rises >= 180 && rises <= 190);
}

TEST(sequence_player_plays_a_replaced_sequence_silently_and_as_before) {
  // The same sequence twice: heard, and with its music replaced.
  Fixture heard, silent;
  std::uint32_t asked_player = 99, asked_starts = 0, asked_data = 0;
  silent.driver.sequences().on_prepare = [&](unsigned player, std::uint32_t starts, std::uint32_t data) {
    asked_player = player;
    asked_starts = starts;
    asked_data = data;
    return true;
  };
  for (Fixture* f : {&heard, &silent}) {
    f->sequence({0x3C, 0x7F, 0x30, 0xFF});
    f->play();
    f->beats(1);
  }
  CHECK(asked_player == 0 && asked_starts == 1 && asked_data == kSequence);
  CHECK(silent.driver.sequences().silent(0) && !heard.driver.sequences().silent(0));
  // The same channel plays the note, at volume 0.
  CHECK(heard.hardware.channel_active(4) && silent.hardware.channel_active(4));
  CHECK((heard.hardware.read(channel(4), 4) & 0x37F) == 127 && (silent.hardware.read(channel(4), 4) & 0x37F) == 0);
  CHECK((heard.hardware.read(channel(4), 4) & ~0x37Fu) == (silent.hardware.read(channel(4), 4) & ~0x37Fu));
  // What the game sees is the same, to the end.
  for (int i = 0; i < 10; ++i) {
    heard.beats(10);
    silent.beats(10);
    CHECK(heard.playing() == silent.playing());
    CHECK(heard.bus.read32(kShared + SoundDriver::kSharedPlayerTicks) == silent.bus.read32(kShared + SoundDriver::kSharedPlayerTicks));
    CHECK(heard.hardware.channel_active(4) == silent.hardware.channel_active(4));
  }
  const SequencePlayer::PlayerInfo info = silent.driver.sequences().player_info(0);
  CHECK(!info.prepared && info.starts == 1 && info.ends == 1);
  CHECK(heard.driver.sequences().player_info(0).ends == 1);
}

TEST(sequence_player_tells_a_stop_from_an_end) {
  Fixture f;
  f.sequence({0x3C, 0x7F, 0x30, 0xFE, 0x00, 0x30, 0x00});  // A note, then waits for ever.
  f.play();
  f.beats(2);
  SequencePlayer::PlayerInfo info = f.driver.sequences().player_info(0);
  CHECK(info.prepared && info.playing && !info.paused && info.starts == 1 && info.ends == 0 && info.decibels == 0);
  // The volume and fader the game sets.
  f.command(SoundDriver::kSetPlayerValue, {0, SequencePlayer::kPlayerVolume, 64, 1});
  f.command(SoundDriver::kSetPlayerValue, {0, SequencePlayer::kPlayerFader, static_cast<std::uint32_t>(-100) & 0xFFFF, 2});
  f.command(SoundDriver::kPauseSequence, {0, 1});
  info = f.driver.sequences().player_info(0);
  CHECK(info.paused && info.decibels == SequencePlayer::decibels_squared(64) - 100);
  // Made heard again: its note follows from the next beat.
  f.driver.sequences().set_silent(0, true);
  CHECK(f.driver.sequences().silent(0));
  f.driver.sequences().set_silent(0, false);
  f.command(SoundDriver::kStopSequence, {0});
  info = f.driver.sequences().player_info(0);
  CHECK(!info.prepared && info.starts == 1 && info.ends == 0);
}
