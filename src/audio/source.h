// A sound read from somewhere (a file of replacement music), a piece at a
// time, as stereo floating-point samples at its own rate.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::audio {

class Source {
 public:
  virtual ~Source() = default;
  virtual int rate() const = 0;
  // Its length in sample frames.
  virtual std::uint64_t length() const = 0;
  // Up to `frames` frames, interleaved left, right; fewer only at the end.
  virtual std::size_t read(float* stereo, std::size_t frames) = 0;
  // To frame `frame`, exactly. False if it cannot.
  virtual bool seek(std::uint64_t frame) = 0;
};

}  // namespace bd::audio
