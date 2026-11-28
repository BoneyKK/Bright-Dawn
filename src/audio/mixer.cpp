#include "audio/mixer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bd::audio {
namespace {

void put_32(std::uint8_t* at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// A WAV header for 16-bit stereo at the mixer's rate, with `bytes` of
// sample data after it.
std::array<std::uint8_t, 44> wav_header(std::uint32_t bytes) {
  std::array<std::uint8_t, 44> h{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                 16,  0,   0,   0,   1, 0, 2, 0, 0,   0,   0,   0,   0,   0,   0,   0,
                                 4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
  put_32(&h[4], 36 + bytes);
  put_32(&h[24], Mixer::kRate);
  put_32(&h[28], Mixer::kRate * 4);
  put_32(&h[40], bytes);
  return h;
}

}  // namespace

Mixer::Mixer() = default;

Mixer::~Mixer() { stop_recording(); }

const std::vector<float>& Mixer::mix() {
  console_rate_.push(console_.data(), console_.size() / 2);
  console_.clear();
  mixed_.clear();
  const std::size_t frames = console_rate_.pull(mixed_, static_cast<std::size_t>(-1));
  for (auto& voice : voices_) voice->add_to(mixed_.data(), frames);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const auto& v) { return v->done(); }), voices_.end());
  for (float& s : mixed_) s = std::clamp(s, -1.0f, 1.0f);

  if (recording_ != nullptr && !mixed_.empty()) {
    std::vector<std::uint8_t> bytes(mixed_.size() * 2);
    for (std::size_t i = 0; i < mixed_.size(); ++i) {
      const auto v = static_cast<std::int16_t>(std::lround(mixed_[i] * 32767.0f));
      bytes[i * 2] = static_cast<std::uint8_t>(v);
      bytes[i * 2 + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
    }
    std::fwrite(bytes.data(), 1, bytes.size(), recording_);
    recorded_ += frames;
  }
  return mixed_;
}

double Mixer::speed(std::size_t queued) {
  const double off = (static_cast<double>(queued) - static_cast<double>(kTargetQueued)) / kTargetQueued;
  return 1.0 + std::clamp(off * 0.005, -0.005, 0.005);
}

bool Mixer::record(const std::filesystem::path& file, std::string& error) {
  stop_recording();
#ifdef _WIN32
  recording_ = _wfopen(file.c_str(), L"wb");
#else
  recording_ = std::fopen(file.c_str(), "wb");
#endif
  if (recording_ == nullptr) {
    error = "cannot write " + file.string();
    return false;
  }
  const auto header = wav_header(0);
  std::fwrite(header.data(), 1, header.size(), recording_);
  recorded_ = 0;
  return true;
}

void Mixer::stop_recording() {
  if (recording_ == nullptr) return;
  const auto header = wav_header(static_cast<std::uint32_t>(std::min<std::uint64_t>(recorded_ * 4, 0xFFFFFFD0u)));
  std::fseek(recording_, 0, SEEK_SET);
  std::fwrite(header.data(), 1, header.size(), recording_);
  std::fclose(recording_);
  recording_ = nullptr;
}

}  // namespace bd::audio
