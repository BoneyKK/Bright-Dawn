#include "nds/sound_driver.h"

#include <algorithm>

namespace bd::nds {

namespace {

constexpr std::uint32_t kControlLow = SoundHardware::kControl;        // Master volume.
constexpr std::uint32_t kControlHigh = SoundHardware::kControl + 1;   // Output choice and the enable bit.
constexpr std::uint32_t kCapture0 = SoundHardware::kCaptureControl;
constexpr std::uint32_t kCapture1 = SoundHardware::kCaptureControl + 1;

// A list longer than this is taken not to end. (The main program has a few
// hundred commands to make lists from.)
constexpr unsigned kLongestList = 65536;

}  // namespace

SoundDriver::SoundDriver(Timeline& timeline, Arm9Bus& bus, SoundHardware& hardware)
    : timeline_(timeline), bus_(bus), hardware_(hardware) {
  timeline_.add(*this);
}

const char* SoundDriver::command_name(std::uint32_t number) {
  static const char* const names[kCommands] = {
      "start a sequence",
      "stop a sequence",
      "prepare a sequence",
      "start a prepared sequence",
      "pause a sequence",
      "skip ahead in a sequence",
      "set a value of a sequence player",
      "set a value of a sequence's tracks",
      "mute a sequence's tracks",
      "choose the channels a sequence player may use",
      "set a sequence player's variable",
      "set a global variable",
      "start channels, capture units and alarms",
      "stop channels, capture units and alarms",
      "set a channel up for sampled sound",
      "set a channel up for a square wave",
      "set a channel up for noise",
      "set a capture unit up",
      "set an alarm up",
      "set channels' timer",
      "set channels' volume",
      "set channels' place between left and right",
      "set the surround effect",
      "set the master volume",
      "set one place for all channels",
      "choose what goes to the outputs",
      "reserve channels",
      "release channels",
      "stop channels that are not reserved",
      "set the shared block",
      "forget sequence data",
      "forget instrument data",
      "forget wave data",
      "read the sound program's state",
  };
  return number < kCommands ? names[number] : "unknown";
}

void SoundDriver::start_up() {
  set_reg(kControlHigh, 1, reg(kControlHigh, 1) | 0x80);
  const std::uint32_t none[4] = {0, 0, 0, 0};
  command(kChooseOutputs, none, 0);
  set_reg(kControlLow, 1, 0x7F);
}

std::uint32_t SoundDriver::memory(std::uint32_t address) {
  if (!Arm9Bus::in_main_memory(address)) {
    ++not_carried_out_["reading outside main memory, which gave 0"];
    return 0;
  }
  return bus_.main_read(address & ~3u, 4);
}

void SoundDriver::set_memory(std::uint32_t address, unsigned size, std::uint32_t value) {
  if (!Arm9Bus::in_main_memory(address)) {
    ++not_carried_out_["writing outside main memory, which was dropped"];
    return;
  }
  address &= ~(size - 1u);
  bus_.main_write(address, size, value);
  if (on_memory_written) on_memory_written(address, size);
}

void SoundDriver::word(std::uint32_t data) {
  if (data >= 0x02000000) {
    ++statistics_.lists_received;
    if (lists_.size() < kListsWaiting) {
      lists_.push_back(data);
    } else {
      ++statistics_.lists_dropped;
    }
    return;
  }
  if (data != 0) return;
  ++statistics_.wake_ups;
  timeline_.refresh();
  work(timeline_.now());
  update_shared();
  timeline_.rescheduled();  // A command may have started an alarm.
}

SoundDriver::Time SoundDriver::next_event() const {
  Time next = next_beat_;
  for (const Alarm& alarm : alarms_) next = std::min(next, alarm.due);
  return next;
}

void SoundDriver::run_until(Time now) {
  for (;;) {
    Time when = next_beat_;
    Alarm* alarm = nullptr;
    for (Alarm& a : alarms_) {
      if (a.due < when) {
        when = a.due;
        alarm = &a;
      }
    }
    if (when > now) break;
    if (alarm != nullptr) {
      fire(*alarm);
    } else {
      ++statistics_.beats;
      next_beat_ += kPeriod;
      work(when);
      update_shared();
    }
  }
}

void SoundDriver::work(Time now) {
  hardware_.run_until(now);
  while (!lists_.empty()) {
    std::uint32_t at = lists_.front();
    lists_.pop_front();
    for (unsigned n = 0; at != 0; ++n) {
      if (n == kLongestList) {
        ++not_carried_out_["a command list that did not end, which was left at its 65,536th command"];
        break;
      }
      if (!Arm9Bus::in_main_memory(at)) {
        ++not_carried_out_["a command list that led outside main memory, which was left there"];
        break;
      }
      // The program copies a command before it carries it out.
      const std::uint32_t next = memory(at + kCommandNext);
      const std::uint32_t number = memory(at + kCommandNumber);
      std::uint32_t arguments[4];
      for (unsigned i = 0; i < 4; ++i) arguments[i] = memory(at + kCommandArguments + 4 * i);
      if (number < kCommands) {
        ++statistics_.commands[number];
        command(number, arguments, now);
      }  // The program skips any other number too.
      at = next;
    }
    ++statistics_.lists_finished;
    if (shared_ == 0) {
      ++not_carried_out_["counting a finished list before the shared block was set"];
    } else {
      set_memory(shared_ + kSharedListsFinished, 4, memory(shared_ + kSharedListsFinished) + 1);
    }
  }
}

void SoundDriver::update_shared() {
  if (shared_ == 0) return;
  std::uint32_t channels = 0, captures = 0;
  for (unsigned i = 0; i < SoundHardware::kChannels; ++i) {
    if ((reg(channel_reg(i, 3), 1) & 0x80) != 0) channels |= 1u << i;
  }
  if ((reg(kCapture0, 1) & 0x80) != 0) captures |= 1;
  if ((reg(kCapture1, 1) & 0x80) != 0) captures |= 2;
  set_memory(shared_ + kSharedChannelsActive, 2, channels);
  set_memory(shared_ + kSharedCapturesActive, 2, captures);
}

void SoundDriver::fire(Alarm& alarm) {
  ++statistics_.alarms_fired;
  alarm.due = alarm.every != 0 ? alarm.due + alarm.every : Timeline::kNever;
  // The program's routine for a firing: nothing if the alarm has been stopped
  // or set up again since; otherwise one more word to send, and as many of
  // those sent as the queue takes.
  if (((alarm.data >> 8) & 0xFF) != alarm.identifier) return;
  if (alarm.unsent < 0xFF) ++alarm.unsent;
  while (alarm.unsent != 0) {
    if (!send || !send(alarm.data)) break;
    --alarm.unsent;
    ++statistics_.alarm_words_sent;
  }
}

void SoundDriver::start_alarm(unsigned n, Time now) {
  Alarm& alarm = alarms_[n];
  alarm.data = n | (static_cast<std::uint32_t>(alarm.identifier) << 8);
  alarm.due = now + alarm.ticks * kCyclesPerTick;
  alarm.every = alarm.between * kCyclesPerTick;
  alarm.set = true;
}

void SoundDriver::stop_alarm(unsigned n) {
  Alarm& alarm = alarms_[n];
  if (!alarm.set) return;
  alarm.due = Timeline::kNever;
  ++alarm.identifier;  // So that a firing already on its way is ignored.
  alarm.set = false;
}

void SoundDriver::stop_channel(unsigned channel, std::uint32_t hold) {
  const std::uint32_t control = reg(channel_reg(channel, 0), 4) & ~SoundHardware::kChannelStart;
  set_reg(channel_reg(channel, 0), 4, (hold & 1) != 0 ? control | 0x8000 : control);
}

std::uint32_t SoundDriver::surround(std::uint32_t volume, std::uint32_t place) const {
  // Sound placed far to one side is made quieter: by up to `decay` parts in
  // 32,767 at the very edge. The program's arithmetic, 32 bits wide.
  const auto p = static_cast<std::int32_t>(place);
  const auto decay = static_cast<std::uint32_t>(surround_decay_);
  std::uint32_t factor;
  if (p < 0x18) {
    factor = decay * (place + 0x28) + ((0x7FFFu - decay) << 6);
  } else if (p <= 0x68) {
    return volume;
  } else {
    factor = (0u - decay) * (place - 0x28) + ((decay + 0x7FFFu) << 6);
  }
  const auto product = static_cast<std::int32_t>(volume * factor);
  // An arithmetic shift, as the program's.
  return static_cast<std::uint32_t>(product < 0 ? ~(~product >> 21) : product >> 21);
}

void SoundDriver::stop_unreserved(std::uint32_t channels) {
  for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
    if ((channels & 1) == 0 || ((reserved_[0] >> i) & 1) != 0) continue;
    // The program also takes the channel away from whatever note of a
    // sequence is playing on it; no sequence plays here.
    stop_channel(i, 0);
  }
}

void SoundDriver::command(std::uint32_t number, const std::uint32_t* a, Time now) {
  const auto bad_channel = [&](std::uint32_t channel) {
    if (channel < SoundHardware::kChannels) return false;
    ++not_carried_out_[std::string("\"") + command_name(number) + "\" for a channel past 15"];
    return true;
  };
  const auto timer_register = [](std::uint32_t timer) { return (0x10000u - timer) & 0xFFFFu; };

  switch (number) {
    case kSetPlayerVariable:
    case kSetGlobalVariable: {
      if (shared_ == 0) {
        ++not_carried_out_["setting a variable before the shared block was set"];
        break;
      }
      if (number == kSetPlayerVariable) {
        set_memory(shared_ + a[0] * kSharedPlayerSize + a[1] * 2 + kSharedPlayerVariables, 2, a[2] & 0xFFFF);
      } else {
        set_memory(shared_ + a[0] * 2 + kSharedGlobalVariables, 2, a[1] & 0xFFFF);
      }
      break;
    }

    case kStart: {
      std::uint32_t channels = a[0];
      for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
        if ((channels & 1) != 0) set_reg(channel_reg(i, 3), 1, reg(channel_reg(i, 3), 1) | 0x80);
      }
      if ((a[1] & 1) != 0) {
        if ((a[1] & 2) != 0) {
          set_reg(kCapture0, 2, reg(kCapture0, 2) | 0x8080);
        } else {
          set_reg(kCapture0, 1, reg(kCapture0, 1) | 0x80);
        }
      } else if ((a[1] & 2) != 0) {
        set_reg(kCapture1, 1, reg(kCapture1, 1) | 0x80);
      }
      std::uint32_t alarms = a[2];
      for (unsigned i = 0; i < kAlarms && alarms != 0; ++i, alarms >>= 1) {
        if ((alarms & 1) != 0) start_alarm(i, now);
      }
      update_shared();
      break;
    }

    case kStop: {
      std::uint32_t alarms = a[2];
      for (unsigned i = 0; i < kAlarms && alarms != 0; ++i, alarms >>= 1) {
        if ((alarms & 1) != 0) stop_alarm(i);
      }
      std::uint32_t channels = a[0];
      for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
        if ((channels & 1) != 0) stop_channel(i, a[3]);
      }
      if ((a[1] & 1) != 0) set_reg(kCapture0, 1, 0);
      if ((a[1] & 2) != 0) set_reg(kCapture1, 1, 0);
      update_shared();
      break;
    }

    case kSetUpSampled:
    case kSetUpSquareWave:
    case kSetUpNoise: {
      // How the arguments are packed differs; what is done with them does
      // not, much.
      const std::uint32_t channel = number == kSetUpSampled ? a[0] & 0xFFFF : a[0];
      if (bad_channel(channel)) break;
      std::uint32_t volume, divider, timer, place, rest;
      if (number == kSetUpSampled) {
        timer = (a[0] >> 16) & 0xFFFF;
        volume = (a[2] >> 24) & 0x7F;
        divider = (a[2] >> 22) & 3;
        place = (a[3] >> 16) & 0x7F;
        const std::uint32_t format = (a[3] >> 24) & 3, repeat = (a[3] >> 26) & 3;
        rest = (repeat << 27) | (format << 29);
      } else {
        volume = a[1] & 0x7F;
        divider = (a[1] >> 8) & 3;
        timer = (a[2] >> 8) & 0xFFFF;
        place = a[2] & 0x7F;
        rest = 0x60000000u | (number == kSetUpSquareWave ? a[3] << 24 : 0u);
      }
      asked_place_[channel] = static_cast<std::uint8_t>(place);
      if (place_for_all_ >= 0) place = static_cast<std::uint32_t>(place_for_all_);
      asked_volume_[channel] = static_cast<std::uint8_t>(volume);
      if (surround_applies(channel)) volume = surround(volume, place);
      set_reg(channel_reg(channel, SoundHardware::kChannelControl), 4, rest | (place << 16) | (divider << 8) | volume);
      set_reg(channel_reg(channel, SoundHardware::kChannelTimer), 2, timer_register(timer));
      if (number == kSetUpSampled) {
        set_reg(channel_reg(channel, SoundHardware::kChannelLoopStart), 2, a[3] & 0xFFFF);
        set_reg(channel_reg(channel, SoundHardware::kChannelLength), 4, a[2] & 0x3FFFFF);
        set_reg(channel_reg(channel, SoundHardware::kChannelSource), 4, a[1] & 0x07FFFFFF);
      }
      break;
    }

    case kSetUpCapture: {
      const std::uint32_t unit = (a[2] >> 31) & 1, eight_bit = (a[2] >> 30) & 1, repeat = (a[2] >> 29) & 1;
      const std::uint32_t from_channel = (a[2] >> 28) & 1, add_to_channel = (a[2] >> 27) & 1;
      set_reg(kCapture0 + unit, 1, ((repeat != 0 ? 0u : 1u) << 2) | (eight_bit << 3) | (from_channel << 1) | add_to_channel);
      set_reg(SoundHardware::kCaptureDestination + 8 * unit, 4, a[0]);
      set_reg(SoundHardware::kCaptureLength + 8 * unit, 2, a[1] & 0xFFFF);
      break;
    }

    case kSetUpAlarm: {
      if (a[0] >= kAlarms) {
        ++not_carried_out_["setting up an alarm past the eighth"];
        break;
      }
      Alarm& alarm = alarms_[a[0]];
      if (alarm.set) {
        alarm.due = Timeline::kNever;
        alarm.set = false;
      }
      alarm.ticks = a[1];
      alarm.between = a[2];
      alarm.identifier = static_cast<std::uint8_t>(a[3]);
      alarm.unsent = 0;
      break;
    }

    case kSetTimer: {
      std::uint32_t channels = a[0];
      for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
        if ((channels & 1) != 0) set_reg(channel_reg(i, SoundHardware::kChannelTimer), 2, timer_register(a[1]));
      }
      break;
    }

    case kSetVolume: {
      std::uint32_t channels = a[0];
      for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
        if ((channels & 1) == 0) continue;
        std::uint32_t volume = a[1];
        asked_volume_[i] = static_cast<std::uint8_t>(volume);
        if (surround_applies(i)) volume = surround(volume, reg(channel_reg(i, 2), 1));
        set_reg(channel_reg(i, 0), 2, (volume | (a[2] << 8)) & 0xFFFF);
      }
      break;
    }

    case kSetPlace: {
      std::uint32_t channels = a[0];
      for (unsigned i = 0; i < SoundHardware::kChannels && channels != 0; ++i, channels >>= 1) {
        if ((channels & 1) == 0) continue;
        std::uint32_t place = a[1];
        asked_place_[i] = static_cast<std::uint8_t>(place);
        if (place_for_all_ >= 0) place = static_cast<std::uint32_t>(place_for_all_);
        set_reg(channel_reg(i, 2), 1, place & 0xFF);
        if (surround_applies(i)) set_reg(channel_reg(i, 0), 1, surround(asked_volume_[i], place) & 0xFF);
      }
      break;
    }

    case kSetSurround:
      surround_decay_ = static_cast<std::int32_t>(a[0]);
      for (unsigned i = 0; i < SoundHardware::kChannels; ++i) {
        if (((kSurroundChannels >> i) & 1) == 0) continue;
        set_reg(channel_reg(i, 0), 1, surround(asked_volume_[i], reg(channel_reg(i, 2), 1)) & 0xFF);
      }
      break;

    case kSetMasterVolume:
      set_reg(kControlLow, 1, a[0] & 0xFF);
      break;

    case kSetPlaceForAll:
      place_for_all_ = static_cast<std::int32_t>(a[0]);
      for (unsigned i = 0; i < SoundHardware::kChannels; ++i)
        set_reg(channel_reg(i, 2), 1, place_for_all_ >= 0 ? a[0] & 0xFF : asked_place_[i]);
      break;

    case kChooseOutputs: {
      const std::uint32_t enable = (reg(kControlHigh, 1) & 0x80) != 0 ? 1 : 0;
      set_reg(kControlHigh, 1, ((enable << 7) | (a[3] << 5) | (a[2] << 4) | (a[1] << 2) | a[0]) & 0xFF);
      break;
    }

    case kReserveChannels:
      stop_unreserved(a[0]);
      reserved_[a[1] & 1] |= a[0];
      break;

    case kReleaseChannels:
      reserved_[a[1] & 1] &= ~a[0];
      break;

    case kStopUnreserved:
      stop_unreserved(a[0]);
      break;

    case kSetSharedBlock:
      shared_ = a[0];
      break;

    default:
      ++not_carried_out_[std::string("\"") + command_name(number) + "\", which is not implemented"];
      break;
  }
}

}  // namespace bd::nds
