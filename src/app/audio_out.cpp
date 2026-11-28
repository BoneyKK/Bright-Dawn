#include "app/audio_out.h"

#include <string>

#include "core/log.h"
#include "audio/mixer.h"

namespace bd::app {

using audio::Mixer;

AudioOut::~AudioOut() {
  if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
  if (tried_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioOut::open() {
  if (tried_) return stream_ != nullptr;
  tried_ = true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    log::warning("sound", std::string("no sound: the audio system cannot be started: ") + SDL_GetError());
    return false;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, Mixer::kRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (stream_ == nullptr) {
    log::warning("sound", std::string("no sound: the audio device cannot be opened: ") + SDL_GetError());
    return false;
  }
  SDL_AudioSpec device{};
  int frames = 0;
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &frames))
    log::info("sound", "playing on " + std::string(SDL_GetCurrentAudioDriver()) + ", " +
                           std::to_string(device.freq) + " Hz, " + std::to_string(device.channels) +
                           " channels, " + std::to_string(frames) + " frames a buffer");
  return true;
}

void AudioOut::set(bool on, int volume) {
  on_ = on && open();
  if (stream_ == nullptr) return;
  SDL_SetAudioStreamGain(stream_, static_cast<float>(volume) / 100.0f);
  if (on_) {
    SDL_ResumeAudioStreamDevice(stream_);
  } else {
    SDL_PauseAudioStreamDevice(stream_);
    SDL_ClearAudioStream(stream_);
  }
}

void AudioOut::play(const std::vector<float>& samples) {
  if (!on_ || samples.empty()) return;
  const int queued_bytes = SDL_GetAudioStreamQueued(stream_);
  const std::size_t queued = queued_bytes > 0 ? static_cast<std::size_t>(queued_bytes) / (2 * sizeof(float)) : 0;
  if (Mixer::too_far_behind(queued)) SDL_ClearAudioStream(stream_);
  SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(Mixer::speed(queued)));
  SDL_PutAudioStreamData(stream_, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
}

}  // namespace bd::app
