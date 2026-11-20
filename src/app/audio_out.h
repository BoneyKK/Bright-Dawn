// The game's sound, played on the computer's audio device through SDL.
//
// The console's sound comes a frame at a time (platform/audio_feed.h). It
// goes into an SDL audio stream at the console's rate; SDL converts it to
// the device's. The stream is played a little faster or slower as its queue
// grows or shrinks, so sound stays in step with the frame clock.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
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
  // A frame's worth of sound, interleaved left, right, 16 bits.
  void play(const std::vector<std::int16_t>& samples);

 private:
  bool open();

  SDL_AudioStream* stream_ = nullptr;
  bool tried_ = false;
  bool on_ = false;
};

}  // namespace bd::app
