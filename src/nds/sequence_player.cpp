#include "nds/sequence_player.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "nds/arm9_bus.h"
#include "nds/sound_driver.h"
#include "nds/sound_hardware.h"

namespace bd::nds {

namespace {

// The order in which channels are tried for a new note, as the program has
// it: the plain sample channels first, the ones that can also play square
// waves and noise last.
constexpr std::array<std::uint8_t, 16> kChannelOrder = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
// Which channels each kind of instrument can use.
constexpr std::uint32_t kSampledChannels = 0xFFFF, kSquareChannels = 0x3F00, kNoiseChannels = 0xC000;
// How fast an attack rises, for the fastest settings (109 to 127), as the
// program has it; slower ones are 255 minus the setting.
constexpr std::array<std::uint8_t, 19> kFastAttacks = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54,
                                                       0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};
// A square wave or noise at its instrument's own key: 440 Hz, eight steps of
// the wave each, in cycles of the sound hardware's clock (33,513,982 / 2 Hz).
constexpr int kSquareTimer = 16756991 / (440 * 8);
// Instrument types.
constexpr std::uint8_t kSampled = 1, kSquare = 2, kNoise = 3, kSampledAt = 4, kDrums = 16, kKeySplit = 17;
// A track that carries out this many events in one tick without waiting is
// taken to be stuck.
constexpr unsigned kMostEventsPerTick = 65536;

int attack_rate(int attack) { return attack >= 109 ? kFastAttacks[static_cast<std::size_t>(127 - attack)] : 255 - attack; }

int fall_rate(int fall) {
  if (fall == 127) return 0xFFFF;
  if (fall == 126) return 0x3C00;
  if (fall < 50) return fall * 2 + 1;
  return 0x1E00 / (126 - fall);
}

std::uint16_t pitch_table(int n) {
  return static_cast<std::uint16_t>(std::lround((std::exp2(n / 768.0) - 1.0) * 65536.0));
}

}  // namespace

// --- The tables ---------------------------------------------------------

int SequencePlayer::decibels_squared(int level) {
  if (level <= 0) return -32768;
  if (level >= 127) return 0;
  return std::max(-722, static_cast<int>(std::lround(400.0 * std::log10(level / 127.0))));
}

std::uint16_t SequencePlayer::channel_volume(int decibels) {
  decibels = std::clamp(decibels, kSilent, 0);
  // The hardware divides by 1, 2, 4 or 16; the volume makes up the rest.
  const int divider = decibels < -240 ? 3 : decibels < -120 ? 2 : decibels < -60 ? 1 : 0;
  const double times = divider == 3 ? 16.0 : static_cast<double>(1 << divider);
  const long volume = std::lround(127.0 * std::pow(10.0, decibels / 200.0) * times);
  return static_cast<std::uint16_t>(std::min(127L, volume) | (divider << 8));
}

std::uint16_t SequencePlayer::timer(int base, int pitch) {
  int octave = 0, within = -pitch;
  while (within < 0) {
    --octave;
    within += 768;
  }
  while (within >= 768) {
    ++octave;
    within -= 768;
  }
  std::uint64_t result = (static_cast<std::uint64_t>(pitch_table(within)) + 0x10000) * static_cast<std::uint64_t>(base);
  const int shift = octave - 16;
  if (shift <= 0) {
    result >>= -shift;
  } else if (shift < 32) {
    if ((result & (~0ull << (32 - shift))) != 0) return 0xFFFF;
    result <<= shift;
  } else {
    return 0xFFFF;
  }
  return static_cast<std::uint16_t>(std::clamp<std::uint64_t>(result, 0x10, 0xFFFF));
}

int SequencePlayer::sine(int index) {
  index &= 127;
  const auto quarter = [](int i) { return static_cast<int>(std::lround(127.0 * std::sin(i * 3.14159265358979323846 / 64.0))); };
  if (index < 32) return quarter(index);
  if (index < 64) return quarter(64 - index);
  if (index < 96) return -quarter(index - 64);
  return -quarter(128 - index);
}

// --- Memory -------------------------------------------------------------

std::uint8_t SequencePlayer::byte(std::uint32_t address) { return static_cast<std::uint8_t>(read(address, 1)); }

std::uint32_t SequencePlayer::read(std::uint32_t address, unsigned size) {
  if (!Arm9Bus::in_main_memory(address)) {
    ++notes_["sound data outside main memory, read as 0"];
    return 0;
  }
  std::uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= driver_.bus_.main_read(address + i, 1) << (8 * i);
  return value;
}

std::uint16_t SequencePlayer::random() {
  random_ = random_ * 1664525u + 1013904223u;
  return static_cast<std::uint16_t>(random_ >> 16);
}

std::uint32_t SequencePlayer::variable_address(unsigned player, unsigned number) const {
  const std::uint32_t shared = driver_.shared_;
  if (shared == 0 || number >= 32) return 0;
  if (number < 16) return shared + SoundDriver::kSharedPlayerVariables + player * SoundDriver::kSharedPlayerSize + number * 2;
  return shared + SoundDriver::kSharedGlobalVariables + (number - 16) * 2;
}

std::int16_t SequencePlayer::variable(unsigned player, unsigned number) {
  const std::uint32_t at = variable_address(player, number);
  if (at == 0) return -1;
  return static_cast<std::int16_t>(read(at, 2));
}

void SequencePlayer::set_variable(unsigned player, unsigned number, std::int32_t value) {
  const std::uint32_t at = variable_address(player, number);
  if (at != 0) driver_.set_memory(at, 2, static_cast<std::uint32_t>(value) & 0xFFFF);
}

// --- Commands -----------------------------------------------------------

void SequencePlayer::command(std::uint32_t number, const std::uint32_t* a) {
  const unsigned player = a[0] & 0xFF;
  const bool good_player = player < kPlayers;
  switch (number) {
    case SoundDriver::kStartSequence:
    case SoundDriver::kPrepareSequence:
      if (!good_player) break;
      prepare(player, a[1], a[2], a[3]);
      if (number == SoundDriver::kStartSequence && players_[player].prepared) players_[player].playing = true;
      return;
    case SoundDriver::kStopSequence:
      if (good_player) finish(player);
      return;
    case SoundDriver::kStartPreparedSequence:
      if (good_player && players_[player].prepared) players_[player].playing = true;
      return;
    case SoundDriver::kPauseSequence:
      if (!good_player) break;
      players_[player].paused = a[1] != 0;
      if (players_[player].paused) {
        for (std::int8_t t : players_[player].tracks) {
          if (t < 0) continue;
          release_notes(tracks_[static_cast<std::size_t>(t)], 127);
          let_go(tracks_[static_cast<std::size_t>(t)]);
        }
      }
      return;
    case SoundDriver::kSkipSequence:
      if (!good_player || !players_[player].prepared) break;
      for (std::uint32_t i = 0; i < a[1]; ++i) {
        if (!step(player, false)) {
          finish(player);
          break;
        }
        ++players_[player].ticks;
      }
      return;
    case SoundDriver::kSetPlayerValue:
      if (!good_player) break;
      for (std::uint32_t i = 0; i < a[3] && i < 4; ++i)
        set_player_byte(players_[player], a[1] + i, static_cast<std::uint8_t>(a[2] >> (8 * i)));
      return;
    case SoundDriver::kSetTrackValue:
    case SoundDriver::kMuteTracks:
    case SoundDriver::kSetPlayerChannels: {
      if (!good_player) break;
      const std::uint32_t mask = a[1];
      for (unsigned i = 0; i < kTracksPerPlayer; ++i) {
        const std::int8_t t = players_[player].tracks[i];
        if (((mask >> i) & 1) == 0 || t < 0) continue;
        Track& track = tracks_[static_cast<std::size_t>(t)];
        if (number == SoundDriver::kSetTrackValue) {
          const std::uint32_t size = a[0] >> 24;
          for (std::uint32_t b = 0; b < size && b < 4; ++b)
            set_track_byte(track, a[2] + b, static_cast<std::uint8_t>(a[3] >> (8 * b)));
        } else if (number == SoundDriver::kMuteTracks) {
          // 0: sound again; 1: no new notes; 2: and let go of the ones
          // playing; 3: and stop them.
          track.muted = a[2] != 0;
          if (a[2] >= 2) {
            release_notes(track, a[2] == 3 ? 127 : -1);
            let_go(track);
          }
        } else {
          track.channel_mask = static_cast<std::uint16_t>(a[2]);
          track.own_channels = true;
        }
      }
      return;
    }
    case SoundDriver::kForgetSequenceData:
    case SoundDriver::kForgetBankData:
      for (unsigned p = 0; p < kPlayers; ++p) {
        const Player& pl = players_[p];
        if (!pl.prepared) continue;
        bool uses = number == SoundDriver::kForgetBankData && pl.bank >= a[0] && pl.bank <= a[1];
        for (std::int8_t t : pl.tracks) {
          if (number != SoundDriver::kForgetSequenceData || t < 0) continue;
          const Track& track = tracks_[static_cast<std::size_t>(t)];
          if (track.at != 0 && track.at >= a[0] && track.at <= a[1]) uses = true;
        }
        if (uses) finish(p);
      }
      return;
    case SoundDriver::kForgetWaveData:
      for (unsigned c = 0; c < kChannels; ++c) {
        const Channel& ch = channels_[c];
        if (!ch.in_use || ch.kind != Kind::kSampled || ch.data < a[0] || ch.data > a[1]) continue;
        stop_hardware(static_cast<int>(c));
        free_channel(static_cast<int>(c));
      }
      return;
    default:
      break;
  }
  ++notes_[std::string("\"") + SoundDriver::command_name(number) + "\" for a player past 15, or not prepared"];
}

void SequencePlayer::set_player_byte(Player& player, std::uint32_t offset, std::uint8_t value) {
  const auto low = [&](auto& field) { field = static_cast<std::remove_reference_t<decltype(field)>>((field & ~0xFF) | value); };
  const auto high = [&](auto& field) {
    field = static_cast<std::remove_reference_t<decltype(field)>>((field & 0xFF) | (value << 8));
  };
  switch (offset) {
    case kPlayerPriority: player.priority = value; break;
    case kPlayerVolume: player.volume = value; break;
    case kPlayerFader: low(player.fader); break;
    case kPlayerFader + 1: high(player.fader); break;
    case kPlayerTempo: low(player.tempo); break;
    case kPlayerTempo + 1: high(player.tempo); break;
    case kPlayerTempoRatio: low(player.tempo_ratio); break;
    case kPlayerTempoRatio + 1: high(player.tempo_ratio); break;
    default: ++notes_["setting a sequence player's value at offset " + std::to_string(offset)]; break;
  }
}

void SequencePlayer::set_track_byte(Track& track, std::uint32_t offset, std::uint8_t value) {
  const auto low = [&](auto& field) { field = static_cast<std::remove_reference_t<decltype(field)>>((field & ~0xFF) | value); };
  const auto high = [&](auto& field) {
    field = static_cast<std::remove_reference_t<decltype(field)>>((field & 0xFF) | (value << 8));
  };
  const auto s8 = static_cast<std::int8_t>(value);
  switch (offset) {
    case 0x01: track.pan_range = value; break;
    case 0x02: low(track.program); break;
    case 0x03: high(track.program); break;
    case 0x04: track.volume = value; break;
    case 0x05: track.volume2 = value; break;
    case 0x06: track.bend = s8; break;
    case 0x07: track.bend_range = value; break;
    case 0x08: track.place = s8; break;
    case 0x09: track.extra_place = s8; break;
    case 0x0A: low(track.fader); break;
    case 0x0B: high(track.fader); break;
    case 0x0C: low(track.extra_pitch); break;
    case 0x0D: high(track.extra_pitch); break;
    case 0x0E: track.attack = value; break;
    case 0x0F: track.decay = value; break;
    case 0x10: track.sustain = value; break;
    case 0x11: track.release = value; break;
    case 0x12: track.priority = value; break;
    case 0x13: track.transpose = s8; break;
    case 0x14: track.portamento_key = value; break;
    case 0x15: track.portamento_time = value; break;
    case 0x16: low(track.sweep); break;
    case 0x17: high(track.sweep); break;
    case 0x18: track.modulation.target = value; break;
    case 0x19: track.modulation.speed = value; break;
    case 0x1A: track.modulation.depth = value; break;
    case 0x1B: track.modulation.range = value; break;
    case 0x1C: low(track.modulation.delay); break;
    case 0x1D: high(track.modulation.delay); break;
    case 0x1E: low(track.channel_mask); break;
    case 0x1F: high(track.channel_mask); break;
    default: ++notes_["setting a track's value at offset " + std::to_string(offset)]; break;
  }
}

// --- Players and tracks -------------------------------------------------

int SequencePlayer::new_track() {
  for (unsigned t = 0; t < kTracks; ++t) {
    if (tracks_[t].in_use) continue;
    tracks_[t] = Track{};
    tracks_[t].in_use = true;
    return static_cast<int>(t);
  }
  ++notes_["a sequence that needed more than the 32 tracks there are"];
  return -1;
}

void SequencePlayer::prepare(unsigned p, std::uint32_t data, std::uint32_t offset, std::uint32_t bank) {
  finish(p);
  Player& player = players_[p];
  player = Player{};
  player.tracks.fill(-1);
  player.bank = bank;
  const int first = new_track();
  if (first < 0) return;
  Track& track = tracks_[static_cast<std::size_t>(first)];
  track.base = data;
  track.at = data + offset;
  player.tracks[0] = static_cast<std::int8_t>(first);
  // A sequence with more than one track says first which ones it uses.
  if (byte(track.at) == 0xFE) {
    const std::uint32_t mask = read(track.at + 1, 2);
    track.at += 3;
    for (unsigned i = 1; i < kTracksPerPlayer; ++i) {
      if (((mask >> i) & 1) == 0) continue;
      const int more = new_track();
      if (more < 0) break;
      tracks_[static_cast<std::size_t>(more)].base = data;
      player.tracks[i] = static_cast<std::int8_t>(more);
    }
  }
  player.prepared = true;
  for (unsigned v = 0; v < 16; ++v) set_variable(p, v, -1);
}

void SequencePlayer::finish(unsigned p) {
  Player& player = players_[p];
  for (std::int8_t& t : player.tracks) {
    if (t < 0) continue;
    close_track(tracks_[static_cast<std::size_t>(t)], p);
    t = -1;
  }
  player.prepared = player.playing = false;
}

void SequencePlayer::close_track(Track& track, unsigned) {
  release_notes(track, -1);
  let_go(track);
  track.in_use = false;
  track.at = 0;
}

void SequencePlayer::release_notes(Track& track, int release) {
  for (int c : track.notes) release_channel(channels_[static_cast<std::size_t>(c)], release);
}

void SequencePlayer::let_go(Track& track) {
  for (int c : track.notes) channels_[static_cast<std::size_t>(c)].track = -1;
  track.notes.clear();
}

int SequencePlayer::find_track(unsigned p, unsigned number) {
  if (number >= kTracksPerPlayer) return -1;
  return players_[p].tracks[number];
}

std::uint32_t SequencePlayer::players_playing() const {
  std::uint32_t playing = 0;
  for (unsigned p = 0; p < kPlayers; ++p)
    if (players_[p].prepared) playing |= 1u << p;
  return playing;
}

void SequencePlayer::beat() {
  for (unsigned p = 0; p < kPlayers; ++p) {
    Player& player = players_[p];
    if (!player.prepared || !player.playing) continue;
    if (!player.paused) {
      int ticks = 0;
      while (player.tempo_counter >= kTickStep) {
        player.tempo_counter = static_cast<std::uint16_t>(player.tempo_counter - kTickStep);
        ++ticks;
      }
      bool ended = false;
      for (int i = 0; i < ticks; ++i) {
        if (!step(p, true)) {
          ended = true;
          break;
        }
        ++player.ticks;
      }
      if (ended) {
        finish(p);
        continue;
      }
      player.tempo_counter = static_cast<std::uint16_t>(player.tempo_counter + ((player.tempo * player.tempo_ratio) >> 8));
    }
    for (std::int8_t t : player.tracks)
      if (t >= 0) update_track(tracks_[static_cast<std::size_t>(t)], player);
  }
  for (unsigned c = 0; c < kChannels; ++c) update_channel(static_cast<int>(c));
}

bool SequencePlayer::step(unsigned p, bool notes) {
  bool any = false;
  for (std::int8_t& t : players_[p].tracks) {
    if (t < 0) continue;
    Track& track = tracks_[static_cast<std::size_t>(t)];
    if (track.at == 0) continue;
    if (step_track(track, p, notes)) {
      any = true;
    } else {
      close_track(track, p);
      t = -1;
    }
  }
  return any;
}

std::int32_t SequencePlayer::argument(Track& track, unsigned p, Argument kind) {
  switch (kind) {
    case Argument::kByte: return byte(track.at++);
    case Argument::kShort: {
      const auto value = static_cast<std::int16_t>(read(track.at, 2));
      track.at += 2;
      return value;
    }
    case Argument::kVariableLength: {
      std::int32_t value = 0;
      for (int i = 0; i < 4; ++i) {
        const std::uint8_t b = byte(track.at++);
        value = (value << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) break;
      }
      return value;
    }
    case Argument::kRandom: {
      const auto low = static_cast<std::int16_t>(read(track.at, 2));
      const auto high = static_cast<std::int16_t>(read(track.at + 2, 2));
      track.at += 4;
      return low + ((static_cast<std::int32_t>(random()) * (high - low + 1)) >> 16);
    }
    case Argument::kVariable: return variable(p, byte(track.at++));
  }
  return 0;
}

bool SequencePlayer::step_track(Track& track, unsigned p, bool notes) {
  for (int c : track.notes) {
    Channel& ch = channels_[static_cast<std::size_t>(c)];
    if (ch.length > 0) --ch.length;
    if (!ch.auto_sweep && ch.sweep_counter < ch.sweep_length) ++ch.sweep_counter;
  }
  if (track.wait_for_notes) {
    if (!track.notes.empty()) return true;
    track.wait_for_notes = false;
  }
  if (track.wait > 0 && --track.wait > 0) return true;

  Player& player = players_[p];
  for (unsigned events = 0; track.wait == 0 && !track.wait_for_notes; ++events) {
    if (events == kMostEventsPerTick) {
      ++notes_["a track that never waited, which was ended"];
      return false;
    }
    std::uint8_t command = byte(track.at++);
    bool carry_out = true, given = false;
    Argument given_kind = Argument::kByte;
    if (command == 0xA2) {
      command = byte(track.at++);
      carry_out = track.condition;
    }
    if (command == 0xA0 || command == 0xA1) {
      given_kind = command == 0xA0 ? Argument::kRandom : Argument::kVariable;
      given = true;
      command = byte(track.at++);
    }
    const auto arg = [&](Argument usual) { return argument(track, p, given ? given_kind : usual); };

    if (command < 0x80) {
      const int velocity = byte(track.at++);
      const std::int32_t length = arg(Argument::kVariableLength);
      if (!carry_out) continue;
      if (notes) note(track, p, command, velocity, length);
      if (track.note_wait) {
        track.wait = length;
        if (length == 0) track.wait_for_notes = true;
      }
      continue;
    }
    switch (command & 0xF0) {
      case 0x80: {
        const std::int32_t value = arg(Argument::kVariableLength);
        if (!carry_out) break;
        if (command == 0x80) track.wait = value;
        else if (command == 0x81 && value < 0x10000) track.program = static_cast<std::uint16_t>(value);
        break;
      }
      case 0x90: {
        if (command == 0x93) {
          const unsigned number = byte(track.at++);
          const std::uint32_t offset = read(track.at, 3);
          track.at += 3;
          if (!carry_out) break;
          const int other = find_track(p, number);
          if (other >= 0 && &tracks_[static_cast<std::size_t>(other)] != &track) {
            Track& opened = tracks_[static_cast<std::size_t>(other)];
            close_track(opened, p);
            opened.in_use = true;
            opened.base = track.base;
            opened.at = track.base + offset;
          }
        } else if (command == 0x94 || command == 0x95) {
          const std::uint32_t offset = read(track.at, 3);
          track.at += 3;
          if (!carry_out) break;
          if (command == 0x95) {
            if (track.depth >= track.returns.size()) break;
            track.loops[track.depth] = 0;
            track.returns[track.depth++] = track.at;
          }
          track.at = track.base + offset;
        } else {
          ++notes_["an unknown sequence event, skipped"];
        }
        break;
      }
      case 0xB0: {
        const unsigned number = byte(track.at++);
        const std::int32_t value = arg(Argument::kShort);
        if (!carry_out) break;
        const std::int32_t was = variable(p, number);
        switch (command) {
          case 0xB0: set_variable(p, number, value); break;
          case 0xB1: set_variable(p, number, was + value); break;
          case 0xB2: set_variable(p, number, was - value); break;
          case 0xB3: set_variable(p, number, was * value); break;
          case 0xB4: if (value != 0) set_variable(p, number, was / value); break;
          case 0xB5: set_variable(p, number, value >= 0 ? was << value : was >> -value); break;
          case 0xB6: {
            const bool negative = value < 0;
            const std::int32_t range = (negative ? -value : value) + 1;
            const std::int32_t r = static_cast<std::int32_t>((static_cast<std::uint32_t>(random()) * range) >> 16);
            set_variable(p, number, negative ? -r : r);
            break;
          }
          case 0xB8: track.condition = was == value; break;
          case 0xB9: track.condition = was >= value; break;
          case 0xBA: track.condition = was > value; break;
          case 0xBB: track.condition = was <= value; break;
          case 0xBC: track.condition = was < value; break;
          case 0xBD: track.condition = was != value; break;
          default: break;
        }
        break;
      }
      case 0xC0:
      case 0xD0: {
        const std::int32_t value = arg(Argument::kByte);
        if (!carry_out) break;
        const auto u8 = static_cast<std::uint8_t>(value);
        switch (command) {
          case 0xC0: track.place = static_cast<std::int8_t>(value - 64); break;
          case 0xC1: track.volume = u8; break;
          case 0xC2: player.volume = u8; break;
          case 0xC3: track.transpose = static_cast<std::int8_t>(value); break;
          case 0xC4: track.bend = static_cast<std::int8_t>(value); break;
          case 0xC5: track.bend_range = u8; break;
          case 0xC6: track.priority = u8; break;
          case 0xC7: track.note_wait = (value & 1) != 0; break;
          case 0xC8:
            track.tie = (value & 1) != 0;
            release_notes(track, -1);
            let_go(track);
            break;
          case 0xC9:
            track.portamento_key = static_cast<std::uint8_t>(value + track.transpose);
            track.portamento = true;
            break;
          case 0xCA: track.modulation.depth = u8; break;
          case 0xCB: track.modulation.speed = u8; break;
          case 0xCC: track.modulation.target = u8; break;
          case 0xCD: track.modulation.range = u8; break;
          case 0xCE: track.portamento = (value & 1) != 0; break;
          case 0xCF: track.portamento_time = u8; break;
          case 0xD0: track.attack = u8; break;
          case 0xD1: track.decay = u8; break;
          case 0xD2: track.sustain = u8; break;
          case 0xD3: track.release = u8; break;
          case 0xD4:
            if (track.depth >= track.returns.size()) break;
            track.loops[track.depth] = u8;
            track.returns[track.depth++] = track.at;
            break;
          case 0xD5: track.volume2 = u8; break;
          default: break;  // 0xD6 shows a variable while debugging.
        }
        break;
      }
      case 0xE0: {
        const std::int32_t value = arg(Argument::kShort);
        if (!carry_out) break;
        if (command == 0xE0) track.modulation.delay = static_cast<std::uint16_t>(value);
        else if (command == 0xE1) player.tempo = static_cast<std::uint16_t>(value);
        else if (command == 0xE3) track.sweep = static_cast<std::int16_t>(value);
        break;
      }
      case 0xF0:
        if (command == 0xFE) {
          track.at += 2;
        } else if (command == 0xFF) {
          if (carry_out) return false;
        } else if (command == 0xFD) {
          if (carry_out && track.depth != 0) track.at = track.returns[--track.depth];
        } else if (command == 0xFC) {
          if (!carry_out || track.depth == 0) break;
          std::uint8_t& count = track.loops[track.depth - 1];
          if (count == 1) {
            --track.depth;
            break;
          }
          if (count != 0) --count;  // 0: for ever.
          track.at = track.returns[track.depth - 1];
        } else {
          ++notes_["an unknown sequence event, skipped"];
        }
        break;
      default:
        ++notes_["an unknown sequence event, skipped"];
        break;
    }
  }
  return true;
}

void SequencePlayer::update_track(Track& track, const Player& player) {
  std::int32_t fader = decibels_squared(track.volume) + decibels_squared(track.volume2) +
                       decibels_squared(player.volume) + track.fader + player.fader;
  fader = std::max(fader, -32768);
  const std::int32_t pitch = track.extra_pitch + ((track.bend * (track.bend_range << 6)) >> 7);
  std::int32_t place = track.place;
  if (track.pan_range != 127) place = (place * track.pan_range + 64) >> 7;
  place = std::clamp(place + track.extra_place, -128, 127);
  for (int c : track.notes) {
    Channel& ch = channels_[static_cast<std::size_t>(c)];
    ch.fader = fader;
    if (ch.stage == Envelope::kRelease) continue;
    ch.user_pitch = pitch;
    ch.user_place = place;
    ch.modulation = track.modulation;
    if (ch.length == 0) release_channel(ch, -1);
  }
}

// --- Notes and instruments ----------------------------------------------

bool SequencePlayer::instrument(std::uint32_t bank, unsigned program, int key, Instrument& out) {
  if (bank == 0 || program >= read(bank + 0x38, 4)) return false;
  const std::uint32_t record = read(bank + 0x3C + 4 * program, 4);
  std::uint8_t type = record & 0xFF;
  std::uint32_t at = bank + (record >> 8);
  if (type == kDrums) {
    const int lowest = byte(at), highest = byte(at + 1);
    if (key < lowest || key > highest) return false;
    at += 2 + static_cast<std::uint32_t>(key - lowest) * 12;
    type = byte(at);
    at += 2;
  } else if (type == kKeySplit) {
    unsigned region = 0;
    while (region < 8 && key > byte(at + region)) ++region;
    if (region == 8) return false;
    at += 8 + region * 12;
    type = byte(at);
    at += 2;
  }
  if (type < kSampled || type > kSampledAt) return false;
  out.type = type;
  out.wave[0] = static_cast<std::uint16_t>(read(at, 2));
  out.wave[1] = static_cast<std::uint16_t>(read(at + 2, 2));
  out.key = byte(at + 4);
  out.attack = byte(at + 5);
  out.decay = byte(at + 6);
  out.sustain = byte(at + 7);
  out.release = byte(at + 8);
  out.place = byte(at + 9);
  return true;
}

std::uint32_t SequencePlayer::wave_address(std::uint32_t bank, unsigned archive, unsigned wave) {
  if (archive >= 4) return 0;
  // The main program puts each wave archive's address into the bank.
  const std::uint32_t arc = read(bank + 0x18 + 8 * archive, 4);
  if (arc == 0 || wave >= read(arc + 0x38, 4)) return 0;
  const std::uint32_t offset = read(arc + 0x3C + 4 * wave, 4);
  if (offset == 0) return 0;
  return offset < 0x02000000 ? arc + offset : offset;
}

void SequencePlayer::note(Track& track, unsigned p, int key, int velocity, std::int32_t length) {
  const Player& player = players_[p];
  key = std::clamp(key + track.transpose, 0, 127);
  if (!track.muted) {
    int c = -1;
    if (track.tie && !track.notes.empty()) {
      c = track.notes.front();
      channels_[static_cast<std::size_t>(c)].key = static_cast<std::uint8_t>(key);
      channels_[static_cast<std::size_t>(c)].velocity = static_cast<std::uint8_t>(velocity);
    }
    if (c < 0) {
      Instrument inst;
      if (!instrument(player.bank, track.program, key, inst)) goto done;
      const std::uint32_t allowed = inst.type == kSquare ? kSquareChannels : inst.type == kNoise ? kNoiseChannels : kSampledChannels;
      const int priority = std::min(255, player.priority + track.priority);
      const int track_number = static_cast<int>(&track - tracks_.data());
      c = take_channel(allowed & track.channel_mask, priority, track.own_channels, track_number);
      if (c < 0) goto done;
      Channel& ch = channels_[static_cast<std::size_t>(c)];
      if (inst.type == kSquare || inst.type == kNoise) {
        ch.kind = inst.type == kSquare ? Kind::kSquare : Kind::kNoise;
        ch.duty = static_cast<std::uint8_t>(inst.wave[0] & 7);
        ch.base_timer = kSquareTimer;
      } else {
        const std::uint32_t wave = inst.type == kSampledAt ? inst.wave[0] | (static_cast<std::uint32_t>(inst.wave[1]) << 16)
                                                            : wave_address(player.bank, inst.wave[1], inst.wave[0]);
        if (wave == 0) {
          free_channel(c);
          goto done;
        }
        ch.kind = Kind::kSampled;
        ch.format = byte(wave) & 3;
        ch.loops = byte(wave + 1);
        ch.base_timer = static_cast<std::uint16_t>(read(wave + 4, 2));
        ch.loop_start = static_cast<std::uint16_t>(read(wave + 6, 2));
        ch.loop_length = read(wave + 8, 4) & 0x3FFFFF;
        ch.data = wave + 12;
      }
      ch.key = static_cast<std::uint8_t>(key);
      ch.base_key = inst.key;
      ch.velocity = static_cast<std::uint8_t>(velocity);
      ch.attack = attack_rate(inst.attack);
      ch.decay = fall_rate(inst.decay);
      ch.sustain = decibels_squared(inst.sustain) * 128;
      ch.release = fall_rate(inst.release);
      ch.place = static_cast<std::int8_t>(inst.place - 64);
      ch.length = track.tie || length == 0 ? -1 : length;
      ch.starting = true;
      track.notes.push_back(c);
    }
    Channel& ch = channels_[static_cast<std::size_t>(c)];
    if (track.attack != 0xFF) ch.attack = attack_rate(track.attack);
    if (track.decay != 0xFF) ch.decay = fall_rate(track.decay);
    if (track.sustain != 0xFF) ch.sustain = decibels_squared(track.sustain) * 128;
    if (track.release != 0xFF) ch.release = fall_rate(track.release);
    ch.sweep = track.sweep;
    if (track.portamento) ch.sweep += (track.portamento_key - key) * 64;
    if (track.portamento_time == 0) {
      ch.sweep_length = length;
      ch.auto_sweep = false;
    } else {
      const std::int64_t time = static_cast<std::int64_t>(track.portamento_time) * track.portamento_time;
      ch.sweep_length = static_cast<std::int32_t>((time * std::abs(ch.sweep)) >> 11);
      ch.auto_sweep = true;
    }
    ch.sweep_counter = 0;
  }
done:
  track.portamento_key = static_cast<std::uint8_t>(key);
}

// --- Channels -----------------------------------------------------------

int SequencePlayer::take_channel(std::uint32_t allowed, int priority, bool strong, int track) {
  allowed &= ~driver_.reserved_[0];
  if (!strong) allowed &= ~driver_.reserved_[1];
  int best = -1;
  const auto priority_of = [&](int c) {
    const Channel& ch = channels_[static_cast<std::size_t>(c)];
    return ch.in_use ? ch.priority : 0;
  };
  for (std::uint8_t c : kChannelOrder) {
    if (((allowed >> c) & 1) == 0) continue;
    if (best < 0 || priority_of(c) < priority_of(best)) {
      best = c;
    } else if (priority_of(c) == priority_of(best) && priority_of(c) != 0 &&
               channels_[c].loudness < channels_[static_cast<std::size_t>(best)].loudness) {
      // Of two as important, the quieter one is taken.
      best = c;
    }
  }
  if (best < 0) return -1;
  Channel& b = channels_[static_cast<std::size_t>(best)];
  if (b.in_use) {
    if (priority < b.priority) return -1;
    stop_hardware(best);
    free_channel(best);
  }
  b = Channel{};
  b.in_use = true;
  b.priority = static_cast<std::uint8_t>(priority);
  b.track = track;
  return best;
}

void SequencePlayer::free_channel(int c) {
  Channel& ch = channels_[static_cast<std::size_t>(c)];
  if (ch.track >= 0) {
    auto& list = tracks_[static_cast<std::size_t>(ch.track)].notes;
    list.erase(std::remove(list.begin(), list.end(), c), list.end());
  }
  ch = Channel{};
}

void SequencePlayer::release_channel(Channel& ch, int release) {
  if (!ch.in_use) return;
  if (release >= 0) ch.release = fall_rate(release);
  ch.priority = 1;
  ch.stage = Envelope::kRelease;
}

void SequencePlayer::take_channels(std::uint32_t channels) {
  for (unsigned c = 0; c < kChannels; ++c)
    if (((channels >> c) & 1) != 0 && channels_[c].in_use) free_channel(static_cast<int>(c));
}

void SequencePlayer::stop_hardware(int c) { driver_.stop_channel(static_cast<unsigned>(c), 0); }

void SequencePlayer::update_channel(int c) {
  Channel& ch = channels_[static_cast<std::size_t>(c)];
  if (!ch.in_use) return;
  const auto unit = static_cast<unsigned>(c);
  const std::uint32_t control_at = SoundDriver::channel_reg(unit, SoundHardware::kChannelControl);
  if (!ch.starting && (driver_.reg(control_at, 4) & SoundHardware::kChannelStart) == 0) {
    // A sound played once has come to its end.
    free_channel(c);
    return;
  }

  // The envelope.
  switch (ch.stage) {
    case Envelope::kAttack:
      ch.level = -((-ch.level * ch.attack) >> 8);
      if (ch.level == 0) ch.stage = Envelope::kDecay;
      break;
    case Envelope::kDecay:
      ch.level -= ch.decay;
      if (ch.level <= ch.sustain) {
        ch.level = ch.sustain;
        ch.stage = Envelope::kSustain;
      }
      break;
    case Envelope::kSustain: break;
    case Envelope::kRelease: ch.level = std::max(ch.level - ch.release, kEnvelopeFloor * 2); break;
  }

  std::int32_t volume = decibels_squared(ch.velocity) + (ch.level >> 7) + ch.fader;
  std::int32_t pitch = (ch.key - ch.base_key) * 64 + ch.user_pitch;
  std::int32_t place = ch.place + ch.user_place;
  if (ch.sweep != 0 && ch.sweep_counter < ch.sweep_length)
    pitch += static_cast<std::int32_t>(static_cast<std::int64_t>(ch.sweep) * (ch.sweep_length - ch.sweep_counter) /
                                       ch.sweep_length);
  // Modulation.
  if (ch.modulation.depth != 0 && ch.modulation_waited >= ch.modulation.delay) {
    const std::int64_t value =
        static_cast<std::int64_t>(sine(ch.modulation_counter >> 8)) * ch.modulation.depth * ch.modulation.range;
    if (ch.modulation.target == 1) volume += static_cast<std::int32_t>((value * 60) >> 14);
    else if (ch.modulation.target == 2) place += static_cast<std::int32_t>((value * 64) >> 14);
    else pitch += static_cast<std::int32_t>((value * 64) >> 14);
  }
  if (ch.modulation_waited < ch.modulation.delay) {
    ++ch.modulation_waited;
  } else {
    ch.modulation_counter = static_cast<std::uint16_t>((ch.modulation_counter + (ch.modulation.speed << 6)) & 0x7FFF);
  }
  if (ch.auto_sweep && ch.sweep_counter < ch.sweep_length) ++ch.sweep_counter;

  ch.loudness = volume;
  if (ch.stage == Envelope::kRelease && volume <= kSilent) {
    stop_hardware(c);
    free_channel(c);
    return;
  }

  // Into the registers.
  std::uint32_t place_register = static_cast<std::uint32_t>(std::clamp(place + 64, 0, 127));
  if (driver_.place_for_all_ >= 0) place_register = static_cast<std::uint32_t>(driver_.place_for_all_) & 0x7F;
  const std::uint16_t loudness = channel_volume(volume);
  std::uint32_t volume_register = loudness & 0x7F;
  if (driver_.surround_applies(unit)) volume_register = driver_.surround(volume_register, place_register) & 0x7F;
  volume_register |= loudness & 0x300;
  std::uint32_t timer_register = (0x10000u - timer(ch.base_timer, pitch)) & 0xFFFF;
  if (ch.kind != Kind::kSampled) timer_register &= 0xFFFC;

  if (ch.starting) {
    ch.starting = false;
    stop_hardware(c);
    std::uint32_t control = volume_register | (place_register << 16) | SoundHardware::kChannelStart;
    if (ch.kind == Kind::kSampled) {
      driver_.set_reg(SoundDriver::channel_reg(unit, SoundHardware::kChannelSource), 4, ch.data & 0x07FFFFFF);
      driver_.set_reg(SoundDriver::channel_reg(unit, SoundHardware::kChannelLoopStart), 2, ch.loop_start);
      driver_.set_reg(SoundDriver::channel_reg(unit, SoundHardware::kChannelLength), 4, ch.loop_length);
      control |= (static_cast<std::uint32_t>(ch.format) << SoundHardware::kFormatShift) |
                 ((ch.loops != 0 ? 1u : 2u) << SoundHardware::kRepeatShift);
    } else {
      control |= 3u << SoundHardware::kFormatShift;
      if (ch.kind == Kind::kSquare) control |= static_cast<std::uint32_t>(ch.duty) << SoundHardware::kDutyShift;
    }
    driver_.set_reg(SoundDriver::channel_reg(unit, SoundHardware::kChannelTimer), 2, timer_register);
    driver_.set_reg(control_at, 4, control);
  } else {
    if (volume_register != ch.written_volume) driver_.set_reg(control_at, 2, volume_register);
    if (place_register != ch.written_place) driver_.set_reg(control_at + 2, 1, place_register);
    if (timer_register != ch.written_timer)
      driver_.set_reg(SoundDriver::channel_reg(unit, SoundHardware::kChannelTimer), 2, timer_register);
  }
  ch.written_volume = volume_register;
  ch.written_place = place_register;
  ch.written_timer = timer_register;
}

}  // namespace bd::nds
