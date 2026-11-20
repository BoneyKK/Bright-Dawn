// The sound program's sequence player: the game's music and most of its
// sound effects.
//
// The main program loads sound data from the cartridge's sound archive
// (SDAT) into main memory and asks the second processor's sound program to
// play it: a sequence (SSEQ; MIDI-like events, read straight from memory),
// with a bank (SBNK; which instrument each program number is, and its
// envelope) whose instruments point into wave archives (SWAR; the sampled
// sounds). The main program has already linked each bank to its wave
// archives in memory; the player only reads.
//
// This is that player, written anew from what the program does: sixteen
// players, thirty-two tracks among them, and the sixteen channels handed out
// to notes as they begin. Each beat of the sound program (5.2 ms), every
// playing player advances by its tempo (48 ticks to a quarter note), its
// tracks carry out their events, and every channel in use has its envelope,
// modulation and sweep brought on and its volume, pitch and place put in
// the sound hardware's registers.
//
// What it was checked against: the decibel table, the sine table, the
// attack table and the order in which channels are handed out are the
// program's own (the formulas below give the same numbers). The pitch and
// volume tables are the console BIOS's, which is not at hand: they are
// computed from what they stand for (a 768th of an octave; a tenth of a
// decibel), and a value can be one step off the BIOS's. Field offsets that
// the main program writes directly (a player's or a track's values) are
// those of the program's structures as the main program uses them; any
// other offset is noted.
//
// Limits:
// - A command asked for while the program reads its own state ("read the
//   sound program's state") is noted and not answered; the game is not
//   known to use it.
// - Skipping ahead in a sequence does not start notes.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bd::nds {

class SoundDriver;

class SequencePlayer {
 public:
  static constexpr unsigned kPlayers = 16;
  static constexpr unsigned kTracks = 32;
  static constexpr unsigned kTracksPerPlayer = 16;
  static constexpr unsigned kChannels = 16;
  // Ticks a quarter note lasts, and the tempo counter's step per tick.
  static constexpr int kTickStep = 240;
  // The quietest a channel can be, in tenths of a decibel, and the envelope's
  // resting level (in 128ths of a tenth).
  static constexpr int kSilent = -723;
  static constexpr int kEnvelopeFloor = kSilent * 128;

  // A player's values the main program sets directly, by their offset in
  // the program's structure for a player.
  static constexpr std::uint32_t kPlayerPriority = 0x04;   // 8 bits.
  static constexpr std::uint32_t kPlayerVolume = 0x05;     // 8 bits.
  static constexpr std::uint32_t kPlayerFader = 0x06;      // 16 bits, in tenths of a decibel.
  static constexpr std::uint32_t kPlayerTempo = 0x18;      // 16 bits.
  static constexpr std::uint32_t kPlayerTempoRatio = 0x1A; // 16 bits, 256 being the tempo as written.

  explicit SequencePlayer(SoundDriver& driver) : driver_(driver) {}

  // The commands for sequences (SoundDriver's numbers 0 to 9 and 0x1E to
  // 0x20). The arguments as the command has them.
  void command(std::uint32_t number, const std::uint32_t* a);
  // One beat of the sound program: the players advance, the channels follow.
  void beat();
  // Channels the main program now drives itself: whatever note of a
  // sequence is on them is let go of.
  void take_channels(std::uint32_t channels);
  // Which players are playing, one bit each; for the shared block.
  std::uint32_t players_playing() const;
  std::uint32_t ticks(unsigned player) const { return players_[player].ticks; }
  bool channel_in_use(unsigned channel) const { return channels_[channel].in_use; }

  // Things the player could not do as the program would, with how often.
  const std::map<std::string, std::uint64_t>& notes() const { return notes_; }

  // The tables, for tests.
  static int decibels_squared(int level);  // 0..127 to tenths of a decibel.
  static std::uint16_t channel_volume(int decibels);  // Volume (bits 0-6) and divider (bits 8-9).
  static std::uint16_t timer(int base, int pitch);  // Pitch in 64ths of a semitone.
  static int sine(int index);  // A whole wave in 128 steps, -127 to 127.

 private:
  enum class Kind : std::uint8_t { kNone, kSampled, kSquare, kNoise };
  enum class Envelope : std::uint8_t { kAttack, kDecay, kSustain, kRelease };

  struct Modulation {
    std::uint8_t target = 0;  // 0: pitch, 1: volume, 2: place.
    std::uint8_t speed = 16, depth = 0, range = 1;
    std::uint16_t delay = 0;
  };

  // What an instrument says for a note.
  struct Instrument {
    std::uint8_t type = 0;  // 1: sampled, 2: square wave, 3: noise, 4: sampled at an address.
    std::uint16_t wave[2] = {0, 0};
    std::uint8_t key = 60, attack = 127, decay = 127, sustain = 127, release = 127, place = 64;
  };

  struct Channel {
    bool in_use = false;
    bool starting = false;   // Its registers are to be set and the channel started.
    int track = -1;          // The track it plays for, or -1.
    Kind kind = Kind::kNone;
    std::uint8_t priority = 0;
    std::uint8_t key = 60, base_key = 60, velocity = 127;
    std::int8_t place = 0;   // The instrument's, around the middle.
    std::int32_t length = -1;  // Ticks until the note is let go of; -1: until the track says.
    // The wave.
    std::uint32_t data = 0;
    std::uint8_t format = 0, loops = 0, duty = 0;
    std::uint16_t base_timer = 0, loop_start = 0;
    std::uint32_t loop_length = 0;
    // The envelope.
    Envelope stage = Envelope::kAttack;
    std::int32_t level = kEnvelopeFloor;
    std::int32_t attack = 0, decay = 0, sustain = 0, release = 0;
    // From the track, each beat.
    std::int32_t fader = 0;
    std::int32_t user_pitch = 0, user_place = 0;
    Modulation modulation;
    std::uint16_t modulation_counter = 0, modulation_waited = 0;
    // Sweep (and portamento).
    std::int32_t sweep = 0, sweep_length = 0, sweep_counter = 0;
    bool auto_sweep = true;
    // How loud it was last (for choosing which to take for a new note).
    std::int32_t loudness = kSilent;
    std::uint32_t written_volume = 0xFFFFFFFF, written_timer = 0xFFFFFFFF, written_place = 0xFFFFFFFF;
  };

  struct Track {
    bool in_use = false;
    std::uint32_t base = 0, at = 0;  // The sequence data, and where the track is in it; at 0: not opened.
    bool note_wait = true, muted = false, tie = false, wait_for_notes = false, portamento = false;
    bool condition = true, own_channels = false;
    std::uint16_t program = 0;
    std::uint8_t volume = 127, volume2 = 127, bend_range = 2, priority = 64, pan_range = 127;
    std::int8_t bend = 0, place = 0, extra_place = 0, transpose = 0;
    std::int16_t fader = 0, extra_pitch = 0, sweep = 0;
    std::uint8_t attack = 0xFF, decay = 0xFF, sustain = 0xFF, release = 0xFF;
    std::uint8_t portamento_key = 60, portamento_time = 0;
    Modulation modulation;
    std::uint16_t channel_mask = 0xFFFF;
    std::int32_t wait = 0;
    std::array<std::uint32_t, 3> returns{};
    std::array<std::uint8_t, 3> loops{};
    unsigned depth = 0;
    std::vector<int> notes;  // Its channels.
  };

  struct Player {
    bool prepared = false, playing = false, paused = false;
    std::uint8_t priority = 64, volume = 127;
    std::int16_t fader = 0;
    std::uint16_t tempo = 120, tempo_ratio = 256, tempo_counter = kTickStep;
    std::uint32_t bank = 0;
    std::array<std::int8_t, kTracksPerPlayer> tracks{};
    std::uint32_t ticks = 0;
  };

  enum class Argument { kByte, kShort, kVariableLength, kRandom, kVariable };

  // Sequences.
  void prepare(unsigned player, std::uint32_t data, std::uint32_t offset, std::uint32_t bank);
  void finish(unsigned player);
  bool step(unsigned player, bool notes);  // One tick; false when every track has ended.
  bool step_track(Track& track, unsigned player, bool notes);  // False when the track has ended.
  int find_track(unsigned player, unsigned number);
  int new_track();
  void close_track(Track& track, unsigned player);
  void release_notes(Track& track, int release);
  void let_go(Track& track);
  void note(Track& track, unsigned player, int key, int velocity, std::int32_t length);
  void update_track(Track& track, const Player& player);
  std::int32_t argument(Track& track, unsigned player, Argument kind);
  std::uint32_t variable_address(unsigned player, unsigned number) const;
  std::int16_t variable(unsigned player, unsigned number);
  void set_variable(unsigned player, unsigned number, std::int32_t value);
  void set_player_byte(Player& player, std::uint32_t offset, std::uint8_t value);
  void set_track_byte(Track& track, std::uint32_t offset, std::uint8_t value);

  // Instruments and waves.
  bool instrument(std::uint32_t bank, unsigned program, int key, Instrument& out);
  std::uint32_t wave_address(std::uint32_t bank, unsigned archive, unsigned wave);

  // Channels.
  int take_channel(std::uint32_t allowed, int priority, bool strong, int track);
  void free_channel(int channel);
  void release_channel(Channel& c, int release);
  void update_channel(int channel);
  void stop_hardware(int channel);

  std::uint8_t byte(std::uint32_t address);
  std::uint32_t read(std::uint32_t address, unsigned size);
  std::uint16_t random();

  SoundDriver& driver_;
  std::array<Player, kPlayers> players_{};
  std::array<Track, kTracks> tracks_{};
  std::array<Channel, kChannels> channels_{};
  std::uint32_t random_ = 0x12345678;
  std::map<std::string, std::uint64_t> notes_;
};

}  // namespace bd::nds
