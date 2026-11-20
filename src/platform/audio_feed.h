// The console's sound, made ready for the computer's audio device.
//
// The sound hardware puts out one sample pair every 1,024 cycles of the
// system clock (32,728 a second), 10 bits each, resting at the bias (0x200).
// AudioFeed turns them into signed 16-bit stereo, a frame's worth at a time,
// and says how fast the audio device should play to stay in step with the
// frame clock: the console's sound is made at the console's rate, the device
// plays at its own, and the two drift apart unless one gives a little.
//
// It can also keep everything in a WAV file, for a report or a test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace bd::platform {

class AudioFeed {
 public:
  // Sample pairs a second: the system clock (33,513,982 Hz) / 1,024.
  static constexpr int kRate = 32728;
  static constexpr int kChannels = 2;
  // The output's resting level, as the game sets the bias register.
  static constexpr int kRest = 0x200;
  // How much the device is kept ahead by: three frames' worth.
  static constexpr std::size_t kTargetQueued = kRate * 3 / 60;

  AudioFeed() = default;
  ~AudioFeed();
  AudioFeed(const AudioFeed&) = delete;
  AudioFeed& operator=(const AudioFeed&) = delete;

  // One sample pair as the sound hardware puts it out.
  void add(std::uint16_t left, std::uint16_t right);
  // The pairs added since the last take, interleaved left, right; taking them
  // empties the feed (and writes them to the recording, if any).
  std::vector<std::int16_t> take();
  std::size_t pairs_waiting() const { return samples_.size() / 2; }

  // How fast the device should play, 1 being the console's rate, given how
  // many pairs it still has queued: a little faster when more than the
  // target is queued, a little slower when less. Never more than half a
  // percent either way, which cannot be heard.
  static double speed(std::size_t queued);
  // Whether the device has fallen so far behind (after a pause, or while
  // running unthrottled) that its queue is better dropped.
  static bool too_far_behind(std::size_t queued) { return queued > kTargetQueued * 8; }

  // Keep everything taken from now on in this WAV file. False, with the
  // reason, if it cannot be made.
  bool record(const std::filesystem::path& file, std::string& error);
  // Finish the WAV file (its sizes are written at the end).
  void stop_recording();
  std::uint64_t pairs_recorded() const { return recorded_; }

 private:
  std::vector<std::int16_t> samples_;
  std::FILE* recording_ = nullptr;
  std::uint64_t recorded_ = 0;
};

}  // namespace bd::platform
