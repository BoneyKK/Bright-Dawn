// The sound hardware: channels, mixer and capture units.
//
// Most tests listen through a capture unit: it records the mixer's output at
// full 16-bit precision, one value per output sample when its timer says so.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/sound_hardware.h"
#include "test.h"

namespace {

using bd::nds::SoundHardware;

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

constexpr std::uint32_t kData = 0x02100000;     // Where the tests keep sound data.
constexpr std::uint32_t kRecorded = 0x02200000; // Where capture unit 0 records,
constexpr std::uint32_t kRecordedRight = 0x02201000;  // and unit 1.
constexpr std::uint32_t kStart = SoundHardware::kChannelStart;
constexpr std::uint32_t kLoop = 1u << SoundHardware::kRepeatShift;
constexpr std::uint32_t kOnce = 2u << SoundHardware::kRepeatShift;
constexpr std::uint32_t kFullVolume = 127;
// A timer value with which a channel takes one step per output sample.
constexpr std::uint16_t kStepPerSample = 0x10000 - SoundHardware::kTimerTicksPerSample;

std::uint32_t channel(unsigned n, std::uint32_t reg) { return SoundHardware::kFirstRegister + 16 * n + reg; }
std::uint32_t format(unsigned f) { return f << SoundHardware::kFormatShift; }

struct Fixture {
  NoIo no_io;
  bd::nds::Arm9Bus bus{no_io};
  SoundHardware sound{bus};
  std::uint64_t samples = 0;

  Fixture() { sound.write(SoundHardware::kControl, 2, SoundHardware::kMasterEnable | 127); }

  // Sets a channel up without starting it.
  void set_up(unsigned n, std::uint32_t control, std::uint32_t source, unsigned loop_start, unsigned length,
              std::uint16_t timer = kStepPerSample) {
    sound.write(channel(n, SoundHardware::kChannelControl), 4, control & ~kStart);
    sound.write(channel(n, SoundHardware::kChannelTimer), 2, timer);
    sound.write(channel(n, SoundHardware::kChannelLoopStart), 2, loop_start);
    sound.write(channel(n, SoundHardware::kChannelLength), 4, length);
    sound.write(channel(n, SoundHardware::kChannelSource), 4, source);
  }
  void start(unsigned n) {
    const std::uint32_t at = channel(n, SoundHardware::kChannelControl) + 3;
    sound.write(at, 1, sound.read(at, 1) | 0x80);
  }
  // Starts both capture units recording 16-bit values, one per output
  // sample, for ever. Their timers are those of channels 1 and 3.
  void listen(unsigned words = 64) {
    sound.write(channel(1, SoundHardware::kChannelTimer), 2, kStepPerSample);
    sound.write(channel(3, SoundHardware::kChannelTimer), 2, kStepPerSample);
    for (unsigned unit = 0; unit < 2; ++unit) {
      sound.write(SoundHardware::kCaptureDestination + 8 * unit, 4, unit == 0 ? kRecorded : kRecordedRight);
      sound.write(SoundHardware::kCaptureLength + 8 * unit, 2, words);
      sound.write(SoundHardware::kCaptureControl + unit, 1, SoundHardware::kCaptureStart);
    }
  }
  void run(std::uint64_t more) {
    samples += more;
    sound.run_until(samples * SoundHardware::kCyclesPerSample);
  }
  // The n-th value recorded (from 0), left or right.
  int heard(unsigned n) { return static_cast<std::int16_t>(bus.read16(kRecorded + 2 * n)); }
  int heard_right(unsigned n) { return static_cast<std::int16_t>(bus.read16(kRecordedRight + 2 * n)); }
};

}  // namespace

TEST(sound_registers_keep_the_bits_that_exist) {
  Fixture f;
  f.sound.write(SoundHardware::kControl, 2, 0);  // Nothing is to move.
  f.sound.write(channel(5, SoundHardware::kChannelControl), 4, 0x7FFFFFFF);
  CHECK(f.sound.read(channel(5, SoundHardware::kChannelControl), 4) == 0x7F7F837F);
  CHECK(!f.sound.channel_active(5));
  f.sound.write(channel(5, SoundHardware::kChannelControl) + 3, 1, 0xFF);
  CHECK(f.sound.channel_active(5));
  CHECK(f.sound.read(channel(5, SoundHardware::kChannelControl) + 3, 1) == 0xFF);
  CHECK(f.sound.read(channel(4, SoundHardware::kChannelControl), 4) == 0);

  // The data address, timer, loop start and length can only be written.
  f.sound.write(channel(5, SoundHardware::kChannelSource), 4, 0xFFFFFFFF);
  f.sound.write(channel(5, SoundHardware::kChannelTimer), 2, 0xFFFF);
  f.sound.write(channel(5, SoundHardware::kChannelLoopStart), 2, 0xFFFF);
  f.sound.write(channel(5, SoundHardware::kChannelLength), 4, 0xFFFFFFFF);
  CHECK(f.sound.read(channel(5, SoundHardware::kChannelSource), 4) == 0);
  CHECK(f.sound.read(channel(5, SoundHardware::kChannelTimer), 4) == 0);
  CHECK(f.sound.read(channel(5, SoundHardware::kChannelLength), 4) == 0);

  f.sound.write(SoundHardware::kControl, 2, 0xFFFF);
  CHECK(f.sound.read(SoundHardware::kControl, 2) == 0xBF7F);
  CHECK(f.sound.read(SoundHardware::kBias, 2) == 0x200);  // As the firmware leaves it.
  f.sound.write(SoundHardware::kBias, 2, 0xFFFF);
  CHECK(f.sound.read(SoundHardware::kBias, 2) == 0x3FF);
  f.sound.write(SoundHardware::kCaptureControl, 2, 0x7F7F);
  CHECK(f.sound.read(SoundHardware::kCaptureControl, 2) == 0x0F0F);
  CHECK(!f.sound.capture_active(0) && !f.sound.capture_active(1));
  f.sound.write(SoundHardware::kCaptureDestination + 8, 4, 0xFFFFFFFF);
  CHECK(f.sound.read(SoundHardware::kCaptureDestination + 8, 4) == 0x07FFFFFC);
  CHECK(f.sound.read(SoundHardware::kCaptureDestination, 4) == 0);
}

TEST(sound_produces_one_sample_every_1024_cycles) {
  Fixture f;
  std::vector<SoundHardware::Output> out;
  f.sound.run_until(1023);
  CHECK(f.sound.statistics().samples == 0);
  f.sound.run_until(1024);
  CHECK(f.sound.statistics().samples == 1);
  f.sound.run_until(1024 * 10 + 5);
  CHECK(f.sound.statistics().samples == 10);
  f.sound.on_output = [&](SoundHardware::Output o) { out.push_back(o); };
  f.sound.run_until(1024 * 13);
  CHECK(f.sound.statistics().samples == 13);
  CHECK(out.size() == 3);
  // With nothing playing the output rests at the bias level.
  CHECK(out[0].left == 0x200 && out[0].right == 0x200);
  CHECK(f.sound.statistics().samples_with_sound == 0);
}

TEST(sound_plays_8_bit_data_once_and_stops) {
  Fixture f;
  f.bus.write32(kData, 0x7FF02010);  // 0x10, 0x20, -0x10, 0x7F.
  f.set_up(0, format(0) | kOnce | kFullVolume, kData, 0, 1);
  f.listen();
  f.start(0);
  CHECK(f.sound.channel_active(0));
  f.run(3);
  CHECK(f.sound.channel_active(0));
  f.run(1);
  CHECK(!f.sound.channel_active(0));
  CHECK((f.sound.read(channel(0, 0), 4) & kStart) == 0);
  f.run(2);
  CHECK(f.heard(0) == 0x1000 && f.heard(1) == 0x2000 && f.heard(2) == -0x1000 && f.heard(3) == 0x7F00);
  CHECK(f.heard(4) == 0 && f.heard(5) == 0);
  CHECK(f.sound.statistics().channels_started == 1 && f.sound.statistics().channels_ended == 1);
  CHECK(f.sound.notes().empty());
}

TEST(sound_loops_16_bit_data_from_the_loop_start) {
  Fixture f;
  f.bus.write16(kData + 0, 100);
  f.bus.write16(kData + 2, 0xFF38);  // -200.
  f.bus.write16(kData + 4, 300);
  f.bus.write16(kData + 6, 0x8000);  // -32,768.
  f.set_up(2, format(1) | kLoop | kFullVolume, kData, 1, 1);
  f.listen();
  f.start(2);
  f.run(8);
  CHECK(f.heard(0) == 100 && f.heard(1) == -200 && f.heard(2) == 300 && f.heard(3) == -32768);
  CHECK(f.heard(4) == 300 && f.heard(5) == -32768 && f.heard(6) == 300 && f.heard(7) == -32768);
  CHECK(f.sound.channel_active(2));
  // Clearing the start bit stops it.
  f.sound.write(channel(2, 0) + 3, 1, 0);
  CHECK(!f.sound.channel_active(2));
  f.run(1);
  CHECK(f.heard(8) == 0);
}

TEST(sound_decodes_compressed_data) {
  // A header (start from level 0, index 0), then the 4-bit values 7, F, 0, 8
  // and four zeros, low half of each byte first. Worked by hand from the
  // format's rule: the difference is the step size times (value's low three
  // bits + a half) / 4, each part rounded down; a set top bit subtracts.
  Fixture f;
  f.bus.write32(kData, 0x00000000);
  f.bus.write32(kData + 4, 0x000080F7);
  f.set_up(0, format(2) | kOnce | kFullVolume, kData, 1, 1);
  f.listen();
  f.start(0);
  f.run(7);
  CHECK(f.sound.channel_active(0));
  f.run(3);
  CHECK(!f.sound.channel_active(0));
  const int expected[] = {0, 11, -19, -15, -18, -15, -12, -10, 0, 0};
  for (unsigned i = 0; i < 10; ++i) CHECK(f.heard(i) == expected[i]);
}

TEST(sound_compressed_data_starts_from_its_header_and_loops_with_the_state_it_had) {
  Fixture f;
  f.bus.write32(kData, 0x00081234);      // Level 0x1234, index 8 (step size 16).
  f.bus.write32(kData + 4, 0x00000091);  // 1, 9, then zeros.
  f.set_up(0, format(2) | kLoop | kFullVolume, kData, 1, 1);
  f.listen();
  f.start(0);
  f.run(12);
  // 1: +(16/8 + 16/4) = +6, index 7 (14). 9: -(14/8 + 14/4) = -4, index 6
  // (13). 0: +13/8 = +1, index 5 (12). 0: +1, index 4 (11). 0: +1 (10), +1
  // (9), +1 (8) ... then back to the loop start with the level and index it
  // had there.
  const int expected[] = {0x1234, 0x123A, 0x1236, 0x1237, 0x1238, 0x1239, 0x123A, 0x123B, 0x1234, 0x123A, 0x1236, 0x1237};
  for (unsigned i = 0; i < 12; ++i) CHECK(f.heard(i) == expected[i]);
  CHECK(f.sound.notes().empty());
}

TEST(sound_compressed_levels_stay_within_16_bits) {
  Fixture f;
  f.bus.write32(kData, 0x00587FF0);      // Level 0x7FF0, the largest step size.
  f.bus.write32(kData + 4, 0x0000FF77);  // Up twice, down twice.
  f.set_up(0, format(2) | kOnce | kFullVolume, kData, 1, 1);
  f.listen();
  f.start(0);
  f.run(5);
  CHECK(f.heard(0) == 0x7FF0 && f.heard(1) == 0x7FFF && f.heard(2) == 0x7FFF);
  // Down by 0x7FFF * 15 / 8 (each part rounded down: 0xFFF + 0x1FFF + 0x3FFF
  // + 0x7FFF = 0xEFFC) to -0x6FFD, and again, which stops at -0x7FFF.
  CHECK(f.heard(3) == -0x6FFD && f.heard(4) == -0x7FFF);
}

TEST(sound_square_waves_are_high_for_their_duty) {
  // Eight steps to a cycle: low first, then high for (setting + 1) of them;
  // setting 7 never goes high.
  const char* shapes[8] = {"_______-", "______--", "_____---", "____----", "___-----", "__------", "_-------", "________"};
  for (unsigned duty = 0; duty < 8; ++duty) {
    Fixture f;
    f.set_up(8, format(3) | (duty << SoundHardware::kDutyShift) | kFullVolume, 0, 0, 0);
    f.listen();
    f.start(8);
    f.run(16);
    for (unsigned i = 0; i < 16; ++i) CHECK(f.heard(i) == (shapes[duty][i % 8] == '-' ? 0x7FFF : -0x7FFF));
    CHECK(f.sound.channel_active(8));
  }
  // Only channels 8 to 13 have one.
  Fixture f;
  f.set_up(7, format(3) | kFullVolume, 0, 0, 0);
  f.listen();
  f.start(7);
  f.run(4);
  CHECK(f.heard(0) == 0 && f.heard(3) == 0);
  CHECK(f.sound.channel_active(7));
}

TEST(sound_noise_follows_its_shift_register) {
  // The register starts at 0x7FFF. Each step shifts it right; a 1 shifted
  // out gives a low level and flips bits 13 and 14, a 0 a high level. By
  // hand: fourteen 1s come out, then fourteen 0s, then a 1, then a 0.
  Fixture f;
  f.set_up(14, format(3) | kFullVolume, 0, 0, 0);
  f.listen();
  f.start(14);
  f.run(31);
  CHECK(f.heard(0) == 0x7FFF);  // Before the first step.
  for (unsigned i = 1; i <= 14; ++i) CHECK(f.heard(i) == -0x7FFF);
  for (unsigned i = 15; i <= 28; ++i) CHECK(f.heard(i) == 0x7FFF);
  CHECK(f.heard(29) == -0x7FFF);
  CHECK(f.heard(30) == 0x7FFF);
}

TEST(sound_channel_timers_set_the_pace) {
  Fixture f;
  for (unsigned i = 0; i < 8; ++i) f.bus.write16(kData + 2 * i, static_cast<std::uint16_t>(10 + i));
  // 256 ticks to a step: two steps per output sample.
  f.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 4, 0xFF00);
  f.listen();
  f.start(0);
  f.run(4);
  CHECK(f.heard(0) == 10 && f.heard(1) == 12 && f.heard(2) == 14 && f.heard(3) == 16);

  Fixture g;
  for (unsigned i = 0; i < 8; ++i) g.bus.write16(kData + 2 * i, static_cast<std::uint16_t>(10 + i));
  // 1,024 ticks to a step: one step every two output samples.
  g.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 4, 0xFC00);
  g.listen();
  g.start(0);
  g.run(5);
  CHECK(g.heard(0) == 10 && g.heard(1) == 10 && g.heard(2) == 11 && g.heard(3) == 11 && g.heard(4) == 12);
}

TEST(sound_volume_divider_and_place_scale_a_channel) {
  Fixture f;
  f.bus.write32(kData, 0x40004000);  // 16,384 twice.
  // Volume 64 of 128, divided by 2, a quarter of the way to the right.
  f.set_up(0, format(1) | kLoop | (1u << SoundHardware::kDividerShift) | (32u << SoundHardware::kPanShift) | 64, kData, 0, 1);
  f.listen();
  f.start(0);
  f.run(1);
  CHECK(f.heard(0) == 3072);        // 16,384 / 2 / 2 * 96/128.
  CHECK(f.heard_right(0) == 1024);  // 16,384 / 2 / 2 * 32/128.

  // The last divider setting is a sixteenth, and place 127 is all the way
  // right.
  f.set_up(0, format(1) | kLoop | (3u << SoundHardware::kDividerShift) | (127u << SoundHardware::kPanShift) | 127, kData, 0, 1);
  f.start(0);
  f.run(1);
  CHECK(f.heard(1) == 0);
  CHECK(f.heard_right(1) == 1024);

  // Channels add up.
  f.set_up(4, format(1) | kLoop | (127u << SoundHardware::kPanShift) | 127, kData, 0, 1);
  f.start(4);
  f.run(1);
  CHECK(f.heard_right(2) == 1024 + 16384);
}

TEST(sound_output_has_ten_bits_around_the_bias) {
  Fixture f;
  std::vector<SoundHardware::Output> out;
  f.sound.on_output = [&](SoundHardware::Output o) { out.push_back(o); };
  f.bus.write32(kData, 0x40004000);
  f.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 1);  // All the way left.
  f.start(0);
  f.run(1);
  CHECK(out[0].left == 0x200 + 0x100 && out[0].right == 0x200);
  CHECK(f.sound.statistics().samples_with_sound == 1);

  f.sound.write(SoundHardware::kControl, 1, 64);  // Half the master volume.
  f.run(1);
  CHECK(out[1].left == 0x200 + 0x80);

  // Loud enough, the output stops at its limits.
  f.sound.write(SoundHardware::kControl, 1, 127);
  f.bus.write32(kData, 0x7FFF7FFF);
  f.set_up(4, format(1) | kLoop | kFullVolume, kData, 0, 1);
  f.start(4);
  f.run(1);
  CHECK(out[2].left == 0x3FF);
  f.bus.write32(kData, 0x80008000);
  f.run(1);
  CHECK(out[3].left == 0);
  CHECK(out[3].right == 0x200);
}

TEST(sound_channels_1_and_3_can_bypass_the_mixer) {
  Fixture f;
  std::vector<SoundHardware::Output> out;
  f.sound.on_output = [&](SoundHardware::Output o) { out.push_back(o); };
  f.bus.write32(kData, 0x40004000);      // For channel 0.
  f.bus.write32(kData + 4, 0x20002000);  // For channel 1,
  f.bus.write32(kData + 8, 0x10001000);  // and 3.
  f.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 1);
  f.set_up(1, format(1) | kLoop | kFullVolume, kData + 4, 0, 1);
  f.set_up(3, format(1) | kLoop | (127u << SoundHardware::kPanShift) | kFullVolume, kData + 8, 0, 1);
  f.listen();
  f.start(0);
  f.start(1);
  f.start(3);
  // Everything through the mixer.
  f.run(1);
  CHECK(f.heard(0) == 0x6000 && f.heard_right(0) == 0x1000);
  CHECK(out[0].left == 0x200 + 0x180 && out[0].right == 0x200 + 0x40);
  // Channels 1 and 3 kept out of the mixer; left output from channel 1 and
  // right output from channel 3. The capture units still record the mixer.
  f.sound.write(SoundHardware::kControl, 2, SoundHardware::kMasterEnable | 0x3000 | (2u << 10) | (1u << 8) | 127);
  f.run(1);
  CHECK(f.heard(1) == 0x4000 && f.heard_right(1) == 0);
  CHECK(out[1].left == 0x200 + 0x80 && out[1].right == 0x200 + 0x40);
  // Both outputs from channels 1 and 3 together.
  f.sound.write(SoundHardware::kControl, 2, SoundHardware::kMasterEnable | 0x3000 | (3u << 10) | (3u << 8) | 127);
  f.run(1);
  CHECK(out[2].left == 0x200 + 0x80 && out[2].right == 0x200 + 0x40);
  // Not kept out, and chosen as well: they count in the mixer and alone.
  f.sound.write(SoundHardware::kControl, 2, SoundHardware::kMasterEnable | (1u << 8) | 127);
  f.run(1);
  CHECK(f.heard(3) == 0x6000);
  CHECK(out[3].left == 0x200 + 0x80 && out[3].right == 0x200 + 0x40);
}

TEST(sound_capture_records_once_or_round_and_round) {
  Fixture f;
  std::vector<std::pair<std::uint32_t, std::size_t>> written;
  f.sound.on_memory_written = [&](std::uint32_t a, std::size_t n) { written.emplace_back(a, n); };
  for (unsigned i = 0; i < 8; ++i) f.bus.write16(kData + 2 * i, static_cast<std::uint16_t>(0x100 * (i + 1)));
  f.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 4);
  f.sound.write(channel(1, SoundHardware::kChannelTimer), 2, kStepPerSample);
  f.sound.write(channel(3, SoundHardware::kChannelTimer), 2, kStepPerSample);
  // Unit 0: one word (two 16-bit values), once.
  f.sound.write(SoundHardware::kCaptureDestination, 4, kRecorded);
  f.sound.write(SoundHardware::kCaptureLength, 2, 1);
  f.sound.write(SoundHardware::kCaptureControl, 1, SoundHardware::kCaptureStart | SoundHardware::kCaptureOnce);
  f.bus.write32(kRecorded + 4, 0xEEEEEEEE);
  f.start(0);
  f.run(1);
  CHECK(f.sound.capture_active(0));
  f.run(1);
  CHECK(!f.sound.capture_active(0));
  CHECK(f.sound.read(SoundHardware::kCaptureControl, 1) == SoundHardware::kCaptureOnce);
  f.run(2);
  CHECK(f.heard(0) == 0x100 && f.heard(1) == 0x200);
  CHECK(f.bus.read32(kRecorded + 4) == 0xEEEEEEEE);
  CHECK(written.size() == 2);
  CHECK(written[0] == std::make_pair(kRecorded, std::size_t{2}));
  CHECK(written[1] == std::make_pair(kRecorded + 2, std::size_t{2}));
  CHECK(f.sound.statistics().bytes_captured == 4);

  // Unit 0 again: 8-bit values, one word, round and round. A length of 0
  // counts as one word.
  f.sound.write(SoundHardware::kCaptureLength, 2, 0);
  f.sound.write(SoundHardware::kCaptureControl, 1, SoundHardware::kCaptureStart | SoundHardware::kCapture8Bit);
  f.run(6);  // Values 5 to 8, then 1 and 2 again.
  CHECK(f.sound.capture_active(0));
  CHECK(f.bus.read8(kRecorded) == 0x01 && f.bus.read8(kRecorded + 1) == 0x02);
  CHECK(f.bus.read8(kRecorded + 2) == 0x07 && f.bus.read8(kRecorded + 3) == 0x08);
  CHECK(f.bus.read32(kRecorded + 4) == 0xEEEEEEEE);
  CHECK(written.back() == std::make_pair(kRecorded + 1, std::size_t{1}));
}

TEST(sound_capture_follows_the_timer_of_its_channel) {
  Fixture f;
  for (unsigned i = 0; i < 8; ++i) f.bus.write16(kData + 2 * i, static_cast<std::uint16_t>(i + 1));
  f.set_up(0, format(1) | kLoop | kFullVolume, kData, 0, 4);
  // Unit 1 goes by channel 3's timer: here one value every two samples.
  f.sound.write(channel(3, SoundHardware::kChannelTimer), 2, 0xFC00);
  f.sound.write(channel(0, SoundHardware::kChannelControl) + 2, 1, 127);  // To the right.
  f.sound.write(SoundHardware::kCaptureDestination + 8, 4, kRecordedRight);
  f.sound.write(SoundHardware::kCaptureLength + 8, 2, 16);
  f.sound.write(SoundHardware::kCaptureControl + 1, 1, SoundHardware::kCaptureStart);
  f.start(0);
  f.run(6);
  CHECK(f.heard_right(0) == 2 && f.heard_right(1) == 4 && f.heard_right(2) == 6);
  CHECK(f.sound.statistics().bytes_captured == 6);
}

TEST(sound_nothing_moves_while_the_master_enable_is_clear) {
  Fixture f;
  f.sound.write(SoundHardware::kControl, 2, 127);
  f.bus.write32(kData, 0x00020001);
  f.set_up(0, format(1) | kOnce | kFullVolume, kData, 0, 1);
  f.listen();
  f.bus.write32(kRecorded, 0xEEEEEEEE);
  f.start(0);
  f.run(10);
  CHECK(f.sound.channel_active(0) && f.sound.capture_active(0));
  CHECK(f.bus.read32(kRecorded) == 0xEEEEEEEE);
  CHECK(f.sound.statistics().samples == 10 && f.sound.statistics().bytes_captured == 0);
  f.sound.write(SoundHardware::kControl, 2, SoundHardware::kMasterEnable | 127);
  f.run(3);
  CHECK(f.heard(0) == 1 && f.heard(1) == 2 && f.heard(2) == 0);
  CHECK(!f.sound.channel_active(0));
}

TEST(sound_notes_what_it_does_not_model) {
  Fixture f;
  // Data that is not in main memory.
  f.set_up(0, format(0) | kOnce | kFullVolume, 0x03000000, 0, 1);
  f.listen();
  f.start(0);
  f.run(4);
  CHECK(f.heard(0) == 0 && f.heard(3) == 0);
  CHECK(!f.sound.channel_active(0));
  CHECK(f.sound.notes().count("sound data was read from outside main memory, as silence") == 1);
  CHECK(f.sound.notes().at("sound data was read from outside main memory, as silence") == 4);

  // The "manual" repeat mode.
  f.set_up(0, format(0) | kFullVolume, kData, 0, 1);
  f.start(0);
  f.run(4);
  CHECK(!f.sound.channel_active(0));
  CHECK(f.sound.notes().count("a channel in the \"manual\" repeat mode reached the end of its data and was stopped") == 1);

  // A capture unit's other source, and its adding to a channel.
  f.sound.write(SoundHardware::kCaptureControl, 1, 0);
  f.bus.write32(kRecorded, 0xEEEEEEEE);
  f.sound.write(SoundHardware::kCaptureControl, 1,
                SoundHardware::kCaptureStart | SoundHardware::kCaptureFromChannel | SoundHardware::kCaptureAddToChannel);
  f.run(2);
  CHECK(f.bus.read32(kRecorded) == 0xEEEEEEEE);
  CHECK(f.sound.notes().size() == 4);
}

namespace {

// Two channels: 16-bit, a step every fourth output sample, a loud sample
// and a quiet one in turn; and 8-bit, a step every sample, on the right.
void enhanced_scene(Fixture& f) {
  for (unsigned i = 0; i < 16; ++i) f.bus.write16(kData + 2 * i, i % 2 == 0 ? 0x4000 : 0xC000);
  for (unsigned i = 0; i < 64; ++i) f.bus.main_write(kData + 0x100 + i, 1, (i * 37) & 0xFF);
  f.set_up(0, kLoop | format(1) | kFullVolume | (32u << SoundHardware::kPanShift), kData, 0, 8,
           0x10000 - SoundHardware::kTimerTicksPerSample * 4);
  f.set_up(2, kLoop | format(0) | kFullVolume | (127u << SoundHardware::kPanShift), kData + 0x100, 0, 16);
  f.listen();
  f.start(0);
  f.start(2);
}

std::vector<SoundHardware::Output> outputs(bool enhanced, std::vector<std::uint8_t>* captured = nullptr) {
  Fixture f;
  f.sound.set_enhanced(enhanced);
  std::vector<SoundHardware::Output> out;
  f.sound.on_output = [&](SoundHardware::Output o) { out.push_back(o); };
  enhanced_scene(f);
  f.run(300);
  if (captured != nullptr)
    for (unsigned i = 0; i < 256; ++i) captured->push_back(static_cast<std::uint8_t>(f.bus.main_read(kRecorded + i, 1)));
  return out;
}

}  // namespace

TEST(sound_enhanced_output_leaves_what_the_ds_makes_as_it_is) {
  std::vector<std::uint8_t> captured_original, captured_enhanced;
  const auto original = outputs(false, &captured_original);
  const auto enhanced = outputs(true, &captured_enhanced);
  REQUIRE(original.size() == 300 && enhanced.size() == 300);
  bool same = true;
  for (std::size_t i = 0; i < 300; ++i)
    same = same && original[i].left == enhanced[i].left && original[i].right == enhanced[i].right;
  CHECK(same);
  CHECK(captured_original == captured_enhanced);
  // Off, there is no enhanced mix.
  CHECK(original[100].fine_left == 0.0f && original[100].fine_right == 0.0f);
}

TEST(sound_enhanced_output_is_smooth_between_samples_and_at_the_same_level) {
  const auto out = outputs(true);
  // As loud as the 10-bit output: the same highest and lowest level, to
  // within its rounding.
  float fine_high = -2, fine_low = 2, ten_high = -2, ten_low = 2;
  for (std::size_t i = 50; i < 300; ++i) {
    fine_high = std::max(fine_high, out[i].fine_left);
    fine_low = std::min(fine_low, out[i].fine_left);
    const float ten = static_cast<float>(static_cast<int>(out[i].left) - 0x200) / 512.0f;
    ten_high = std::max(ten_high, ten);
    ten_low = std::min(ten_low, ten);
  }
  CHECK(std::abs(fine_high - ten_high) < 2.0f / 512 && std::abs(fine_low - ten_low) < 2.0f / 512);
  // The 10-bit left side takes two levels; the enhanced one many, on the
  // way between them.
  std::vector<std::uint16_t> held;
  std::vector<float> smooth;
  for (std::size_t i = 50; i < 300; ++i) {
    if (std::find(held.begin(), held.end(), out[i].left) == held.end()) held.push_back(out[i].left);
    if (std::find(smooth.begin(), smooth.end(), out[i].fine_left) == smooth.end()) smooth.push_back(out[i].fine_left);
  }
  CHECK(held.size() <= 3 && smooth.size() >= 4);
  // No step larger than the hold has: from loud to quiet over four samples.
  float largest = 0;
  for (std::size_t i = 51; i < 300; ++i) largest = std::max(largest, std::abs(out[i].fine_left - out[i - 1].fine_left));
  float held_step = 0;
  for (std::size_t i = 51; i < 300; ++i)
    held_step = std::max(held_step, static_cast<float>(std::abs(out[i].left - out[i - 1].left)) / 512.0f);
  CHECK(largest < held_step * 0.75f);
}
