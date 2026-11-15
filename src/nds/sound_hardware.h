// The DS's sound hardware: sixteen channels, a mixer and two capture units.
//
// Each channel plays one thing: sampled sound from memory (8-bit, 16-bit or
// 4-bit compressed), a square wave (channels 8 to 13) or noise (14 and 15),
// at its own rate, volume and place between left and right. The mixer adds
// the channels up. A capture unit records what the mixer puts out into
// memory, which is how games add echo: they record, change the recording and
// play it back through channel 1 or 3.
//
// Only the second processor can reach these registers (0x04000400 to
// 0x0400051F on its side), so nothing the main program does touches them
// directly: they are driven by the stand-in for the second processor's sound
// program.
//
// What each register does is taken from GBATEK's description of the DS sound
// hardware. The arithmetic of mixing (how far a sample is scaled at each
// step, and where it is rounded) is the arithmetic NooDS uses. Where the two
// reference emulators at hand differ, GBATEK decides: the compressed format
// is decoded with a set top bit meaning "subtract", and a square wave with
// duty setting 7 stays low.
//
// The hardware is brought up to date in steps of one output sample (1,024
// cycles of the system clock, 32,728 samples a second). Within a step a
// channel moves through its data by its own timer, exactly.
//
// Limits, none of which the game's sound program is known to depend on:
// - A channel starts at once. On the hardware the first sample comes a few
//   samples late.
// - A channel that is stopped contributes nothing; the "hold" bit, which
//   keeps the last level, is not modelled.
// - While the master enable bit is clear nothing moves, and turning it on
//   continues where things were.
// - Writing a channel's data address while it plays does not restart it.
// - A capture unit records the mixer only. Its other source (a single
//   channel) records nothing here, and the bit that adds the capture to a
//   channel's output does nothing; both are noted when used.
// - Sound data outside main memory reads as silence, and is noted.
// - What a channel in the "manual" repeat mode does at the end of its data is
//   not known here; it is stopped, and noted.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "nds/arm9_bus.h"
#include "nds/timeline.h"

namespace bd::nds {

class SoundHardware : public Timeline::Source {
 public:
  using Time = Timeline::Time;

  static constexpr unsigned kChannels = 16;
  static constexpr unsigned kCaptureUnits = 2;
  static constexpr std::uint32_t kFirstRegister = 0x04000400;
  static constexpr std::uint32_t kPastRegisters = 0x04000520;
  // One output sample, in cycles of the system clock.
  static constexpr Time kCyclesPerSample = 1024;
  // The sound hardware's own clock ticks once every two system clock cycles,
  // so a channel's timer is advanced by this much per output sample.
  static constexpr unsigned kTimerTicksPerSample = 512;

  // Registers, by their offset from a channel's first (channel n is at
  // 0x04000400 + 16 n).
  static constexpr std::uint32_t kChannelControl = 0x0;  // 32 bits.
  static constexpr std::uint32_t kChannelSource = 0x4;   // 32 bits: where the data is.
  static constexpr std::uint32_t kChannelTimer = 0x8;    // 16 bits: counts up from here to 0x10000 per step.
  static constexpr std::uint32_t kChannelLoopStart = 0xA;  // 16 bits, in 4-byte words.
  static constexpr std::uint32_t kChannelLength = 0xC;   // 32 bits, in words, after the loop start.
  static constexpr std::uint32_t kControl = 0x04000500;  // 16 bits: master volume, output choice, enable.
  static constexpr std::uint32_t kBias = 0x04000504;     // 16 bits: the output's resting level.
  static constexpr std::uint32_t kCaptureControl = 0x04000508;  // One byte per unit.
  static constexpr std::uint32_t kCaptureDestination = 0x04000510;  // 32 bits; unit 1 is 8 further.
  static constexpr std::uint32_t kCaptureLength = 0x04000514;       // 16 bits, in words.

  // Bits of a channel's control register.
  static constexpr std::uint32_t kChannelStart = 0x80000000;  // Set to start; reads as set while playing.
  static constexpr unsigned kFormatShift = 29;  // 0: 8-bit, 1: 16-bit, 2: compressed, 3: square wave or noise.
  static constexpr unsigned kRepeatShift = 27;  // 0: manual, 1: loop for ever, 2: play once.
  static constexpr unsigned kDutyShift = 24;    // Square wave: high for (n + 1) eighths; 7: never.
  static constexpr unsigned kPanShift = 16;     // 0 left to 127 right.
  static constexpr unsigned kDividerShift = 8;  // 0: full, 1: half, 2: a quarter, 3: a sixteenth.
  // Bits of the control register.
  static constexpr std::uint32_t kMasterEnable = 0x8000;
  // Bits of a capture unit's control register.
  static constexpr std::uint32_t kCaptureStart = 0x80;
  static constexpr std::uint32_t kCapture8Bit = 0x08;
  static constexpr std::uint32_t kCaptureOnce = 0x04;
  static constexpr std::uint32_t kCaptureFromChannel = 0x02;
  static constexpr std::uint32_t kCaptureAddToChannel = 0x01;

  // What goes to the console's two digital-to-analogue converters for one
  // output sample: 10 bits each, resting at the bias register's value.
  struct Output {
    std::uint16_t left = 0, right = 0;
  };

  struct Statistics {
    std::uint64_t samples = 0;             // Output samples produced.
    std::uint64_t samples_with_sound = 0;  // Of those, the ones not at the resting level on both sides.
    std::uint64_t bytes_captured = 0;
    std::uint64_t channels_started = 0;
    std::uint64_t channels_ended = 0;      // Played once and reached their end.
  };

  // `bus` is where sound data is read from and captures are written to (main
  // memory, as the second processor sees it); it must outlive this object.
  explicit SoundHardware(Arm9Bus& bus);

  // Register access as the second processor has it. `size` is 1, 2 or 4.
  // Registers that can only be written read as 0.
  std::uint32_t read(std::uint32_t address, unsigned size) const;
  void write(std::uint32_t address, unsigned size, std::uint32_t value);

  bool channel_active(unsigned channel) const { return (channels_[channel].control & kChannelStart) != 0; }
  bool capture_active(unsigned unit) const { return (captures_[unit].control & kCaptureStart) != 0; }

  // Timeline::Source. The hardware does nothing at a time of its own that
  // anything else has to wait for: it is brought up to date whenever time
  // stops for another event, and by whoever is about to use it.
  Time next_event() const override { return Timeline::kNever; }
  void run_until(Time now) override;

  // Called with every output sample, in order.
  std::function<void(Output)> on_output;
  // Called when a capture unit has written to memory: address, bytes.
  std::function<void(std::uint32_t, std::size_t)> on_memory_written;

  const Statistics& statistics() const { return statistics_; }
  // Things that happened which this model does not cover (see the limits
  // above), in words, with how often.
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

 private:
  struct Channel {
    std::uint32_t control = 0, source = 0, length = 0;
    std::uint16_t timer = 0, loop_start = 0;
    // While playing.
    std::uint32_t position = 0;  // Address of the data being played.
    std::uint32_t counter = 0;   // The timer's count.
    std::int32_t value = 0;      // Compressed format: the level reached,
    int index = 0;               // the step size's index,
    bool high_half = false;      // which half of the byte is next,
    std::int32_t loop_value = 0; // and the first two as they were at the loop start.
    int loop_index = 0;
    unsigned duty_step = 0;      // Square wave: place in its eight steps.
    std::uint16_t noise = 0;     // Noise: the shift register,
    bool noise_low = false;      // and the level it last gave.
  };
  struct Capture {
    std::uint8_t control = 0;
    std::uint32_t destination = 0;
    std::uint16_t length = 0;
    std::uint32_t position = 0;
    std::uint32_t counter = 0;
  };

  std::uint8_t read_byte(std::uint32_t address) const;
  void write_byte(std::uint32_t address, std::uint8_t value);
  void start(unsigned channel);
  void sample();
  // The level channel `i` gives now, as a signed 16-bit value.
  std::int32_t level(unsigned i, Channel& c);
  // Moves channel `i` on by one step of its timer. False if it ended.
  bool step(unsigned i, Channel& c);
  void capture(Capture& c, const Channel& timer_from, std::int64_t mixed);
  std::uint32_t data(std::uint32_t address, unsigned size);
  bool anything_running() const;
  Output resting() const;

  Arm9Bus& bus_;
  std::array<Channel, kChannels> channels_{};
  std::array<Capture, kCaptureUnits> captures_{};
  std::uint16_t control_ = 0;
  std::uint16_t bias_ = 0x200;  // As the firmware leaves it.
  Time next_sample_ = kCyclesPerSample;
  Statistics statistics_;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
