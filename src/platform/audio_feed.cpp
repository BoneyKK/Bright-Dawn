#include "platform/audio_feed.h"

#include <algorithm>
#include <array>

namespace bd::platform {
namespace {

std::int16_t to_16_bits(std::uint16_t level) {
  // 10 bits around the resting level: 64 times as many steps.
  const int value = (static_cast<int>(level & 0x3FF) - AudioFeed::kRest) * 64;
  return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

void put_32(std::uint8_t* at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// A WAV header for 16-bit stereo at the console's rate, with `bytes` of
// sample data after it.
std::array<std::uint8_t, 44> wav_header(std::uint32_t bytes) {
  std::array<std::uint8_t, 44> h{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                 16,  0,   0,   0,   1, 0, 2, 0, 0,   0,   0,   0,   0,   0,   0,   0,
                                 4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
  put_32(&h[4], 36 + bytes);
  put_32(&h[24], AudioFeed::kRate);
  put_32(&h[28], AudioFeed::kRate * 4);
  put_32(&h[40], bytes);
  return h;
}

}  // namespace

AudioFeed::~AudioFeed() { stop_recording(); }

void AudioFeed::add(std::uint16_t left, std::uint16_t right) {
  samples_.push_back(to_16_bits(left));
  samples_.push_back(to_16_bits(right));
}

std::vector<std::int16_t> AudioFeed::take() {
  std::vector<std::int16_t> taken;
  taken.swap(samples_);
  if (recording_ != nullptr && !taken.empty()) {
    // Little-endian, as WAV wants.
    std::vector<std::uint8_t> bytes(taken.size() * 2);
    for (std::size_t i = 0; i < taken.size(); ++i) {
      bytes[i * 2] = static_cast<std::uint8_t>(taken[i]);
      bytes[i * 2 + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(taken[i]) >> 8);
    }
    std::fwrite(bytes.data(), 1, bytes.size(), recording_);
    recorded_ += taken.size() / 2;
  }
  return taken;
}

double AudioFeed::speed(std::size_t queued) {
  const double off = (static_cast<double>(queued) - static_cast<double>(kTargetQueued)) / kTargetQueued;
  return 1.0 + std::clamp(off * 0.005, -0.005, 0.005);
}

bool AudioFeed::record(const std::filesystem::path& file, std::string& error) {
  stop_recording();
  recording_ = std::fopen(file.string().c_str(), "wb");
  if (recording_ == nullptr) {
    error = "cannot write " + file.string();
    return false;
  }
  const auto header = wav_header(0);
  std::fwrite(header.data(), 1, header.size(), recording_);
  recorded_ = 0;
  return true;
}

void AudioFeed::stop_recording() {
  if (recording_ == nullptr) return;
  const auto header = wav_header(static_cast<std::uint32_t>(std::min<std::uint64_t>(recorded_ * 4, 0xFFFFFFD0u)));
  std::fseek(recording_, 0, SEEK_SET);
  std::fwrite(header.data(), 1, header.size(), recording_);
  std::fclose(recording_);
  recording_ = nullptr;
}

}  // namespace bd::platform
