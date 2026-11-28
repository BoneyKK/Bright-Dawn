#include "audio/music_voice.h"

#include <algorithm>

namespace bd::audio {

MusicVoice::MusicVoice(std::unique_ptr<Source> source, Loop loop, int out_rate)
    : source_(std::move(source)),
      loop_(loop),
      resampler_(source_->rate(), out_rate),
      step_(static_cast<float>(1.0 / (kRampSeconds * out_rate))) {
  if (loop_.end == 0 || loop_.end > source_->length()) loop_.end = source_->length();
  if (loop_.start >= loop_.end) loop_.start = 0;
}

std::size_t MusicVoice::read(float* stereo, std::size_t frames) {
  std::size_t done = 0;
  // Twice at most: once up to the loop's end, once from its start.
  for (int pass = 0; pass < 3 && done < frames && !ended_; ++pass) {
    if (loop_.on && position_ >= loop_.end) {
      if (!source_->seek(loop_.start)) {
        ended_ = true;
        break;
      }
      position_ = loop_.start;
    }
    std::size_t want = frames - done;
    if (loop_.on) want = static_cast<std::size_t>(std::min<std::uint64_t>(want, loop_.end - position_));
    const std::size_t got = source_->read(stereo + done * 2, want);
    done += got;
    position_ += got;
    if (got < want) {
      // The file ends before the loop says it does: go back now, or stop.
      if (loop_.on && got == 0 && position_ < loop_.end) loop_.end = position_;
      if (!loop_.on) ended_ = true;
      if (loop_.on && position_ <= loop_.start) ended_ = true;
    }
  }
  return done;
}

void MusicVoice::add_to(float* out, std::size_t frames) {
  if (done_) return;
  if (!started_) {
    gain_ = target_;
    started_ = true;
  }
  if (paused_ && fade_ <= 0) return;
  resampled_.clear();
  std::size_t have = 0;
  while (have < frames) {
    const std::size_t need = resampler_.needed_for(frames - have);
    if (need > 0) {
      if (ended_) break;
      read_.resize(need * 2);
      const std::size_t got = read(read_.data(), need);
      resampler_.push(read_.data(), got);
      if (got == 0) continue;
    }
    have += resampler_.pull(resampled_, frames - have);
  }
  for (std::size_t i = 0; i < have; ++i) {
    if (gain_ < target_) gain_ = std::min(target_, gain_ + step_);
    else if (gain_ > target_) gain_ = std::max(target_, gain_ - step_);
    if (paused_ || stopping_) fade_ = std::max(0.0f, fade_ - step_);
    else fade_ = std::min(1.0f, fade_ + step_);
    const float g = gain_ * fade_;
    out[i * 2] += resampled_[i * 2] * g;
    out[i * 2 + 1] += resampled_[i * 2 + 1] * g;
    if (paused_ && fade_ <= 0) break;
  }
  if ((stopping_ && fade_ <= 0) || (ended_ && have < frames)) done_ = true;
}

}  // namespace bd::audio
