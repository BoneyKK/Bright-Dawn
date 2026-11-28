// A change of sample rate for stereo sound: windowed-sinc interpolation.
//
// The console makes its sound at 32,728 samples a second; replacement music
// comes at 44,100 or 48,000; the mixer works at 48,000 (mixer.h). Each
// stream is brought to the mixer's rate here. An output sample is the sum of
// the 32 input samples around its place, each weighted by a sinc function
// under a Kaiser window, its cutoff at 95% of the lower of the two Nyquist
// frequencies; the weights for 512 places between two input samples are
// worked out once, and those between them interpolated. At the same rate
// in and out, samples pass unchanged.
//
// Input is pushed and output pulled as they come; what is still needed for
// the next output samples is kept. The output is 16 input samples late.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bd::audio {

class Resampler {
 public:
  static constexpr int kHalfWidth = 16;   // Input samples on each side of an output sample.
  static constexpr int kPhases = 512;     // Places between two input samples with weights of their own.
  static constexpr double kCutoff = 0.95; // Of the lower Nyquist frequency.

  Resampler(double from_rate, double to_rate);

  // Interleaved left, right.
  void push(const float* stereo, std::size_t frames);
  // Appends up to `most` output frames to `out` (interleaved); returns how many.
  std::size_t pull(std::vector<float>& out, std::size_t most);
  // Input frames needed before `frames` more output frames can be pulled.
  std::size_t needed_for(std::size_t frames) const;
  // Forgets everything pushed, as if new.
  void reset();

  double from_rate() const { return from_; }
  double to_rate() const { return to_; }

 private:
  double from_, to_;
  double step_;        // Input frames per output frame.
  bool same_ = false;  // The same rate in and out: samples pass as they are.
  std::vector<float> table_;  // (kPhases + 1) rows of 2 kHalfWidth weights.
  std::vector<float> input_;  // Interleaved, from the oldest sample still needed.
  double at_ = 0;             // Where the next output sample is, in input frames from input_'s start.
};

}  // namespace bd::audio
