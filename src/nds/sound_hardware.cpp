#include "nds/sound_hardware.h"

#include <algorithm>

namespace bd::nds {

namespace {

// Which bits of each register exist.
constexpr std::uint32_t kChannelControlBits = 0xFF7F837F;
constexpr std::uint32_t kAddressBits = 0x07FFFFFC;
constexpr std::uint32_t kLengthBits = 0x003FFFFF;
constexpr std::uint32_t kControlBits = 0xBF7F;
constexpr std::uint32_t kBiasBits = 0x03FF;
constexpr std::uint32_t kCaptureControlBits = 0x8F;

constexpr std::int32_t kHigh = 0x7FFF, kLow = -0x7FFF;

// The compressed format (IMA ADPCM): the step size for each index, and how
// the index moves with the low three bits of each 4-bit value.
constexpr std::int16_t kStepSizes[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF};
constexpr int kIndexMoves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// Replaces byte `at` of `old` with `value`, keeping only the bits in `bits`.
template <typename T>
T with_byte(T old, unsigned at, std::uint8_t value, std::uint32_t bits) {
  const std::uint32_t mask = 0xFFu << (8 * at);
  return static_cast<T>(((old & ~mask) | (static_cast<std::uint32_t>(value) << (8 * at))) & bits);
}

std::uint8_t byte_of(std::uint32_t value, unsigned at) { return static_cast<std::uint8_t>(value >> (8 * at)); }

// A volume, place or master volume of 127 counts as 128: full.
std::int64_t out_of_128(std::uint32_t seven_bits) { return seven_bits == 127 ? 128 : seven_bits; }

}  // namespace

SoundHardware::SoundHardware(Arm9Bus& bus) : bus_(bus) {}

std::uint8_t SoundHardware::read_byte(std::uint32_t address) const {
  if (address >= kFirstRegister && address < 0x04000500) {
    const Channel& c = channels_[(address >> 4) & 0xF];
    const unsigned at = address & 0xF;
    return at < 4 ? byte_of(c.control, at) : std::uint8_t{0};  // The rest can only be written.
  }
  switch (address) {
    case kControl: return byte_of(control_, 0);
    case kControl + 1: return byte_of(control_, 1);
    case kBias: return byte_of(bias_, 0);
    case kBias + 1: return byte_of(bias_, 1);
    case kCaptureControl: return captures_[0].control;
    case kCaptureControl + 1: return captures_[1].control;
    default: break;
  }
  if (address >= kCaptureDestination && address < kPastRegisters) {
    const Capture& c = captures_[(address >> 3) & 1];
    const unsigned at = address & 7;
    return at < 4 ? byte_of(c.destination, at) : std::uint8_t{0};
  }
  return 0;
}

std::uint32_t SoundHardware::read(std::uint32_t address, unsigned size) const {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(read_byte(address + i)) << (8 * i);
  return value;
}

void SoundHardware::write(std::uint32_t address, unsigned size, std::uint32_t value) {
  // Lowest byte first, so that a whole control register is in place by the
  // time its top byte, with the start bit, is looked at.
  for (unsigned i = 0; i < size; ++i) write_byte(address + i, byte_of(value, i));
}

void SoundHardware::write_byte(std::uint32_t address, std::uint8_t value) {
  if (address >= kFirstRegister && address < 0x04000500) {
    const unsigned channel = (address >> 4) & 0xF;
    Channel& c = channels_[channel];
    const unsigned at = address & 0xF;
    if (at < 4) {
      const std::uint32_t old = c.control;
      c.control = with_byte(old, at, value, kChannelControlBits);
      if ((old & kChannelStart) == 0 && (c.control & kChannelStart) != 0) start(channel);
    } else if (at < 8) {
      c.source = with_byte(c.source, at - 4, value, kAddressBits);
    } else if (at < 10) {
      c.timer = with_byte(c.timer, at - 8, value, 0xFFFF);
    } else if (at < 12) {
      c.loop_start = with_byte(c.loop_start, at - 10, value, 0xFFFF);
    } else {
      c.length = with_byte(c.length, at - 12, value, kLengthBits);
    }
    return;
  }
  if (address == kControl || address == kControl + 1) {
    control_ = with_byte(control_, address - kControl, value, kControlBits);
    return;
  }
  if (address == kBias || address == kBias + 1) {
    bias_ = with_byte(bias_, address - kBias, value, kBiasBits);
    return;
  }
  if (address == kCaptureControl || address == kCaptureControl + 1) {
    const unsigned unit = address - kCaptureControl;
    Capture& c = captures_[unit];
    const auto now = static_cast<std::uint8_t>(value & kCaptureControlBits);
    if ((c.control & kCaptureStart) == 0 && (now & kCaptureStart) != 0) {
      c.position = c.destination;
      c.counter = channels_[1 + 2 * unit].timer;
      if ((now & kCaptureFromChannel) != 0)
        ++notes_["a capture unit was started with a single channel as its source, which records nothing here"];
      if ((now & kCaptureAddToChannel) != 0)
        ++notes_["a capture unit was started with the bit that adds it to a channel's output, which does nothing here"];
    }
    c.control = now;
    return;
  }
  if (address >= kCaptureDestination && address < kPastRegisters) {
    Capture& c = captures_[(address >> 3) & 1];
    const unsigned at = address & 7;
    if (at < 4) {
      c.destination = with_byte(c.destination, at, value, kAddressBits);
    } else if (at < 6) {
      c.length = with_byte(c.length, at - 4, value, 0xFFFF);
    }
    return;
  }
  // 0x04000502-3, 0x04000506-7 and 0x0400050A-F hold nothing.
}

void SoundHardware::start(unsigned channel) {
  Channel& c = channels_[channel];
  ++statistics_.channels_started;
  c.position = c.source;
  c.counter = c.timer;
  switch ((c.control >> kFormatShift) & 3) {
    case 2: {
      // The data begins with a header: the level and the index to start from.
      const std::uint32_t header = data(c.source, 4);
      c.value = static_cast<std::int16_t>(header);
      c.index = std::min(static_cast<int>((header >> 16) & 0x7F), 88);
      c.high_half = false;
      c.loop_value = 0;
      c.loop_index = 0;
      c.position += 4;
      break;
    }
    case 3:
      c.duty_step = 0;
      c.noise = 0x7FFF;
      c.noise_low = false;
      break;
    default:
      break;
  }
}

std::uint32_t SoundHardware::data(std::uint32_t address, unsigned size) {
  if (!Arm9Bus::in_main_memory(address)) {
    ++notes_["sound data was read from outside main memory, as silence"];
    return 0;
  }
  return bus_.main_read(address, size);
}

std::int32_t SoundHardware::level(unsigned i, Channel& c) {
  switch ((c.control >> kFormatShift) & 3) {
    case 0: return static_cast<std::int8_t>(data(c.position, 1)) * 256;
    case 1: return static_cast<std::int16_t>(data(c.position, 2));
    case 2: return c.value;
    default: break;
  }
  if (i >= 8 && i <= 13) {
    // Low for the first (7 - n) of eight steps, then high; never high for 7.
    const unsigned duty = (c.control >> kDutyShift) & 7;
    return duty != 7 && c.duty_step >= 7 - duty ? kHigh : kLow;
  }
  if (i >= 14) return c.noise_low ? kLow : kHigh;
  return 0;  // Channels 0 to 7 have neither a square wave nor noise.
}

bool SoundHardware::step(unsigned i, Channel& c) {
  const unsigned format = (c.control >> kFormatShift) & 3;
  switch (format) {
    case 0:
      c.position += 1;
      break;
    case 1:
      c.position += 2;
      break;
    case 2: {
      if (c.position == c.source + c.loop_start * 4u && !c.high_half) {
        c.loop_value = c.value;
        c.loop_index = c.index;
      }
      const std::uint32_t byte = data(c.position, 1);
      const std::uint32_t four_bits = c.high_half ? (byte >> 4) & 0xF : byte & 0xF;
      const std::int32_t size = kStepSizes[c.index];
      std::int32_t difference = size / 8;
      if (four_bits & 1) difference += size / 4;
      if (four_bits & 2) difference += size / 2;
      if (four_bits & 4) difference += size;
      if (four_bits & 8) {
        c.value = std::max(c.value - difference, kLow);
      } else {
        c.value = std::min(c.value + difference, kHigh);
      }
      c.index = std::clamp(c.index + kIndexMoves[four_bits & 7], 0, 88);
      c.high_half = !c.high_half;
      if (!c.high_half) c.position += 1;
      break;
    }
    default:
      if (i >= 8 && i <= 13) {
        c.duty_step = (c.duty_step + 1) % 8;
      } else if (i >= 14) {
        // The bit shifted out decides the level, and whether the register is
        // changed.
        c.noise_low = (c.noise & 1) != 0;
        c.noise >>= 1;
        if (c.noise_low) c.noise ^= 0x6000;
      }
      return true;  // These have no end.
  }
  if (c.position < c.source + (c.loop_start + c.length) * 4u) return true;
  const unsigned repeat = (c.control >> kRepeatShift) & 3;
  if (repeat == 1) {
    if (format == 2 && c.loop_start == 0)
      ++notes_["a compressed sound looped back into its own header"];
    c.position = c.source + c.loop_start * 4u;
    if (format == 2) {
      c.value = c.loop_value;
      c.index = c.loop_index;
      c.high_half = false;
    }
    return true;
  }
  if (repeat != 2) ++notes_["a channel in the \"manual\" repeat mode reached the end of its data and was stopped"];
  c.control &= ~kChannelStart;
  ++statistics_.channels_ended;
  return false;
}

void SoundHardware::capture(Capture& c, const Channel& timer_from, std::int64_t mixed) {
  c.counter += kTimerTicksPerSample;
  while (c.counter >= 0x10000) {
    c.counter = c.counter - 0x10000 + timer_from.timer;
    if ((c.control & kCaptureFromChannel) == 0) {
      const std::int64_t level = std::clamp<std::int64_t>(mixed, -0x800000, 0x7FFFFF);
      const unsigned size = (c.control & kCapture8Bit) != 0 ? 1 : 2;
      if (Arm9Bus::in_main_memory(c.position)) {
        bus_.main_write(c.position, size, static_cast<std::uint32_t>(size == 1 ? level >> 16 : level >> 8));
        if (on_memory_written) on_memory_written(c.position, size);
        statistics_.bytes_captured += size;
      } else {
        ++notes_["a capture unit was pointed outside main memory, and what it captured was dropped"];
      }
      c.position += size;
    } else {
      c.position += (c.control & kCapture8Bit) != 0 ? 1 : 2;
    }
    // A length of 0 counts as one word.
    const std::uint32_t bytes = std::max<std::uint32_t>(c.length, 1) * 4u;
    if (c.position >= c.destination + bytes) {
      if ((c.control & kCaptureOnce) != 0) {
        c.control = static_cast<std::uint8_t>(c.control & 0x7Fu);
        return;
      }
      c.position = c.destination;
    }
  }
}

bool SoundHardware::anything_running() const {
  if ((control_ & kMasterEnable) == 0) return false;
  for (const Channel& c : channels_)
    if ((c.control & kChannelStart) != 0) return true;
  for (const Capture& c : captures_)
    if ((c.control & kCaptureStart) != 0) return true;
  return false;
}

SoundHardware::Output SoundHardware::resting() const { return Output{bias_, bias_}; }

void SoundHardware::sample() {
  ++statistics_.samples;
  if (!anything_running()) {
    if (on_output) on_output(resting());
    return;
  }

  // Levels carry 8 bits below the point from here to the master volume.
  std::int64_t mixer_left = 0, mixer_right = 0;
  std::int64_t alone_left[2] = {0, 0}, alone_right[2] = {0, 0};  // Channels 1 and 3 by themselves.
  for (unsigned i = 0; i < kChannels; ++i) {
    Channel& c = channels_[i];
    if ((c.control & kChannelStart) == 0) continue;
    std::int64_t value = level(i, c);

    c.counter += kTimerTicksPerSample;
    while (c.counter >= 0x10000) {
      c.counter = c.counter - 0x10000 + c.timer;
      if (!step(i, c)) break;
    }

    const unsigned divider = (c.control >> kDividerShift) & 3;
    value <<= 4 - (divider == 3 ? 4 : divider);
    value *= out_of_128(c.control & 0x7F);
    const std::int64_t pan = out_of_128((c.control >> kPanShift) & 0x7F);
    const std::int64_t left = (value * (128 - pan) / 128) >> 3;
    const std::int64_t right = (value * pan / 128) >> 3;

    if (i == 1 || i == 3) {
      alone_left[i >> 1] = left;
      alone_right[i >> 1] = right;
      // Bits 12 and 13 keep channels 1 and 3 out of the mixer.
      if ((control_ & (0x1000u << (i >> 1))) != 0) continue;
    }
    mixer_left += left;
    mixer_right += right;
  }

  for (unsigned unit = 0; unit < kCaptureUnits; ++unit) {
    Capture& c = captures_[unit];
    if ((c.control & kCaptureStart) != 0) capture(c, channels_[1 + 2 * unit], unit == 0 ? mixer_left : mixer_right);
  }

  const auto chosen = [&](unsigned choice, std::int64_t mixer, const std::int64_t* alone) {
    switch (choice & 3) {
      case 0: return mixer;
      case 1: return alone[0];
      case 2: return alone[1];
      default: return alone[0] + alone[1];
    }
  };
  const std::int64_t master = out_of_128(control_ & 0x7F);
  const auto converted = [&](std::int64_t level) {
    const std::int64_t sixteen_bits = (level * master / 128) >> 8;
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>((sixteen_bits >> 6) + bias_, 0, 0x3FF));
  };
  const Output output{converted(chosen(control_ >> 8, mixer_left, alone_left)),
                      converted(chosen(control_ >> 10, mixer_right, alone_right))};
  if (output.left != bias_ || output.right != bias_) ++statistics_.samples_with_sound;
  if (on_output) on_output(output);
}

void SoundHardware::run_until(Time now) {
  while (next_sample_ <= now) {
    if (!on_output && !anything_running()) {
      // Nothing to do but count the samples up to now.
      const Time samples = (now - next_sample_) / kCyclesPerSample + 1;
      statistics_.samples += samples;
      next_sample_ += samples * kCyclesPerSample;
      break;
    }
    sample();
    next_sample_ += kCyclesPerSample;
  }
}

}  // namespace bd::nds
