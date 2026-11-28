// Replacement music in place of the game's sequences.
//
// The game's main program asks the sound program to play a sequence by
// where its events are in memory; it gives no name. The sequences of the
// sound archive in the player's ROM are known by name (rom/sdat.h): the one
// asked for is found by its events, which are the same bytes. Where the
// music pack (music_pack.h) has a file for it, the file plays, and the
// sequence is played silently: it still runs, note for note, so that
// whatever the game reads of it (which players are playing, the ticks,
// the variables a sequence sets) is as before.
//
// What the game does to the sequence it does to the file: it starts when
// the sequence starts, pauses with it, follows its player's volume and
// fader (so the game's fades fade it) and the master volume, and stops when
// the game stops the sequence. When the sequence ends by itself (a fanfare,
// which does not loop), the file plays on to its end. The tempo the game
// sets, and what it changes of single tracks, do not reach the file.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/mixer.h"
#include "audio/music_pack.h"
#include "audio/music_voice.h"
#include "rom/sdat.h"

namespace bd::audio {

// The sequences of the sound archive, to tell which one is in memory.
class SequenceCatalog {
 public:
  SequenceCatalog() = default;
  explicit SequenceCatalog(const std::vector<rom::SdatSequence>& sequences);

  // The name of the sequence whose events start at `at` of `memory`, or an
  // empty string. The one with the lowest number of those that match.
  std::string identify(ByteSpan memory, std::size_t at) const;
  std::size_t size() const { return sequences_.size(); }

 private:
  // The bytes looked at first: a sequence's beginning.
  static constexpr std::size_t kKeyBytes = 16;
  struct Entry {
    std::string name;
    std::vector<std::uint8_t> data;
  };
  static std::uint64_t key(const std::uint8_t* data, std::size_t size);
  std::vector<Entry> sequences_;
  std::unordered_multimap<std::uint64_t, std::size_t> by_key_;
};

// A sequence player, as the sound program has it (nds/sequence_player.h).
struct PlayerState {
  bool prepared = false, playing = false, paused = false;
  std::uint32_t starts = 0;  // Sequences prepared on it so far.
  std::uint32_t ends = 0;    // Of those, the ones that ended by themselves.
  int decibels = 0;          // Its volume and fader, in tenths of a decibel.
};

class MusicReplacer {
 public:
  static constexpr unsigned kPlayers = 16;
  // How loud a file plays, against the console's full scale, at a
  // player's full volume. The game's music, as its sequences play it,
  // reaches 0.7 to 0.9 of the console's full scale, at about -15 dB RMS
  // (measured on the opening's music, see docs/DEVELOPMENT.md); music files
  // are made about as loud, so a file plays at its own level, and the
  // player's volume and fader on top.
  static constexpr float kFileLevel = 1.0f;

  MusicReplacer(SequenceCatalog catalog, MusicPack pack);

  // When a sequence is prepared on `player` (the `starts`-th there), from
  // events at `at` of main memory: whether the pack replaces it, in which
  // case the sequence is to be played silently.
  bool prepared(unsigned player, std::uint32_t starts, ByteSpan memory, std::size_t at);
  // Each frame, after the console's: the players as they are now, and the
  // master volume (1 being full). Starts, follows and stops the music.
  void update(const std::array<PlayerState, kPlayers>& players, float master, Mixer& mixer);
  // The players whose music could not be played after all: their sequences
  // are to be heard again. Taking them clears them.
  std::uint32_t take_failed();
  // Whether the pack is used (Settings, Sound), from the next sequence.
  // Off, sequences are still told by name, for the log.
  void set_enabled(bool on) { enabled_ = on; }
  // Of every piece of music, 1 being its usual level (Settings, Sound).
  void set_volume(float volume) { volume_ = volume; }
  // Stops all music (the pack switched off); the sequences are left silent
  // until they end.
  void stop_all();

  const MusicPack& pack() const { return pack_; }
  const SequenceCatalog& catalog() const { return catalog_; }
  // What is playing for a player: the sequence's name, or empty.
  const std::string& playing(unsigned player) const { return current_[player].name; }

 private:
  struct Slot {
    std::uint32_t starts = 0;
    std::string name;
    const PackTrack* track = nullptr;
    std::shared_ptr<MusicVoice> voice;
    bool in_mixer = false;
    std::uint32_t ends = 0;  // The player's `ends` when it started.
    bool fresh = false;      // Not yet seen in update().
  };

  SequenceCatalog catalog_;
  MusicPack pack_;
  std::array<Slot, kPlayers> pending_{}, current_{};
  std::uint32_t failed_ = 0;
  float volume_ = 1.0f;
  bool enabled_ = true;
};

}  // namespace bd::audio
