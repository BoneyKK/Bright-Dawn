// One piece of replacement music as it plays: read from its source, looped
// where its loop says, brought to the mixer's rate, and made louder or
// softer, paused and stopped smoothly.
//
// The loop is exact: the sample before the loop's end is followed by the
// one at its start. A change of loudness is spread over 10 ms, and so are a
// pause and a stop, so none of them clicks.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "audio/music_pack.h"
#include "audio/resampler.h"
#include "audio/source.h"

namespace bd::audio {

class MusicVoice {
 public:
  static constexpr double kRampSeconds = 0.010;

  MusicVoice(std::unique_ptr<Source> source, Loop loop, int out_rate);

  // How loud, 1 being as the file is. Reached over kRampSeconds.
  void set_gain(float gain) { target_ = gain; }
  void set_paused(bool paused) { paused_ = paused; }
  // Plays on to the end of the file, without looping again.
  void finish() { loop_.on = false; }
  // Goes quiet over kRampSeconds, then is done.
  void stop() { stopping_ = true; }

  // Adds `frames` frames of it to `out` (interleaved stereo).
  void add_to(float* out, std::size_t frames);
  bool done() const { return done_; }
  // Where it is in its file, in frames (for tests and the log).
  std::uint64_t position() const { return position_; }

 private:
  // Reads up to `frames` frames from the source, following the loop.
  std::size_t read(float* stereo, std::size_t frames);

  std::unique_ptr<Source> source_;
  Loop loop_;
  Resampler resampler_;
  std::uint64_t position_ = 0;
  bool ended_ = false;  // The source has nothing more.
  float gain_ = 0, target_ = 1;
  float step_;          // The most the gain moves in a frame.
  float fade_ = 1;      // For pausing and stopping.
  bool paused_ = false, stopping_ = false, done_ = false;
  bool started_ = false;
  std::vector<float> read_, resampled_;
};

}  // namespace bd::audio
