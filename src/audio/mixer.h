// The program's sound, as the audio device gets it: stereo, 32-bit floating
// point, 48,000 samples a second.
//
// Two kinds of sound go in. The console's (its sound hardware's output,
// 32,728 samples a second) is brought to 48,000 by a windowed-sinc
// resampler (resampler.h). Replacement music (music_voice.h) plays beside
// it, each piece from its own file at its own rate. The console's sound sets
// the pace: each frame, as many samples are mixed as the console's sound of
// that frame covers, and every piece of music moves on by as much, so music
// keeps time with the game when the game runs slow.
//
// The mix is limited to full scale. It can be kept in a WAV file (16 bits),
// for a report or a test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/music_voice.h"
#include "audio/resampler.h"

namespace bd::audio {

class Mixer {
 public:
  static constexpr int kRate = 48000;
  static constexpr int kConsoleRate = 32728;  // The system clock (33,513,982 Hz) / 1,024.
  // How much the device is kept ahead by: three frames' worth.
  static constexpr std::size_t kTargetQueued = kRate * 3 / 60;

  Mixer();
  ~Mixer();
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;

  // One sample pair of the console's sound, full scale being 1.
  void add_console(float left, float right) {
    console_.push_back(left);
    console_.push_back(right);
  }
  std::size_t console_waiting() const { return console_.size() / 2; }
  // A piece of music to play until it is done.
  void play(std::shared_ptr<MusicVoice> voice) { voices_.push_back(std::move(voice)); }
  std::size_t voices() const { return voices_.size(); }

  // Everything the console's sound added since the last mix covers, with
  // the music beside it, interleaved left, right. The console's samples are
  // used up (and the mix written to the recording, if any).
  const std::vector<float>& mix();

  // How fast the device should play, 1 being 48,000 a second, given how
  // many frames it still has queued: a little faster when more than the
  // target is queued, a little slower when less. Never more than half a
  // percent either way, which cannot be heard.
  static double speed(std::size_t queued);
  // Whether the device has fallen so far behind (after a pause, or while
  // running unthrottled) that its queue is better dropped.
  static bool too_far_behind(std::size_t queued) { return queued > kTargetQueued * 8; }

  // Keep everything mixed from now on in this WAV file. False, with the
  // reason, if it cannot be made.
  bool record(const std::filesystem::path& file, std::string& error);
  void stop_recording();
  std::uint64_t frames_recorded() const { return recorded_; }

 private:
  Resampler console_rate_{kConsoleRate, kRate};
  std::vector<float> console_, resampled_, mixed_;
  std::vector<std::shared_ptr<MusicVoice>> voices_;
  std::FILE* recording_ = nullptr;
  std::uint64_t recorded_ = 0;
};

}  // namespace bd::audio
