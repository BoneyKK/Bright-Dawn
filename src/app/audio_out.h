// The program's sound, played on the computer's audio device through SDL.
//
// The mix comes a frame at a time (audio/mixer.h): stereo, 32-bit floating
// point, 48,000 samples a second. It goes into an SDL audio stream, which
// converts it to the device's format and rate where they differ. The stream
// is played a little faster or slower as its queue grows or shrinks, so
// sound stays in step with the frame clock.
#pragma once

#include <SDL3/SDL.h>

#include <vector>

namespace bd::app {

class AudioOut {
 public:
  AudioOut() = default;
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  // On or off (Settings, Sound); the device is opened when first turned on.
  // `volume` in percent.
  void set(bool on, int volume);
  // A frame's worth of sound, interleaved left, right.
  void play(const std::vector<float>& samples);

 private:
  bool open();

  SDL_AudioStream* stream_ = nullptr;
  bool tried_ = false;
  bool on_ = false;
};

}  // namespace bd::app
