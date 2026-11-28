#include "audio/resampler.h"

#include <algorithm>
#include <cmath>

namespace bd::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
// The Kaiser window's shape: about 90 dB down outside the pass band.
constexpr double kKaiserBeta = 8.6;

// The modified Bessel function of the first kind, order 0, as its series.
double bessel_i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 64; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

}  // namespace

Resampler::Resampler(double from_rate, double to_rate) : from_(from_rate), to_(to_rate), step_(from_rate / to_rate) {
  same_ = from_rate == to_rate;
  if (same_) return;
  const double cutoff = kCutoff * std::min(1.0, to_rate / from_rate);
  const int width = 2 * kHalfWidth;
  table_.assign(static_cast<std::size_t>(kPhases + 1) * width, 0.0f);
  const double window_norm = bessel_i0(kKaiserBeta);
  for (int p = 0; p <= kPhases; ++p) {
    const double frac = static_cast<double>(p) / kPhases;
    double weights[2 * kHalfWidth];
    double sum = 0;
    for (int k = 0; k < width; ++k) {
      // Input sample k is at distance d from the output sample.
      const double d = (k - (kHalfWidth - 1)) - frac;
      const double x = d / kHalfWidth;
      const double window = std::abs(x) >= 1 ? 0 : bessel_i0(kKaiserBeta * std::sqrt(1 - x * x)) / window_norm;
      const double t = cutoff * d;
      const double sinc = t == 0 ? 1 : std::sin(kPi * t) / (kPi * t);
      weights[k] = cutoff * sinc * window;
      sum += weights[k];
    }
    // Each row adds up to 1: a steady level passes unchanged.
    for (int k = 0; k < width; ++k) table_[static_cast<std::size_t>(p) * width + k] = static_cast<float>(weights[k] / sum);
  }
  reset();
}

void Resampler::reset() {
  input_.clear();
  at_ = 0;
  // Silence before the first sample, for the first output samples' weights.
  if (!same_) input_.assign(static_cast<std::size_t>(kHalfWidth - 1) * 2, 0.0f);
  at_ = same_ ? 0 : kHalfWidth - 1;
}

void Resampler::push(const float* stereo, std::size_t frames) { input_.insert(input_.end(), stereo, stereo + frames * 2); }

std::size_t Resampler::needed_for(std::size_t frames) const {
  const std::size_t have = input_.size() / 2;
  if (same_) return frames > have ? frames - have : 0;
  if (frames == 0) return 0;
  // The last output sample needs input up to its place plus kHalfWidth.
  const double last = at_ + step_ * static_cast<double>(frames - 1);
  const auto need = static_cast<std::size_t>(std::floor(last)) + kHalfWidth + 1;
  return need > have ? need - have : 0;
}

std::size_t Resampler::pull(std::vector<float>& out, std::size_t most) {
  const std::size_t have = input_.size() / 2;
  if (same_) {
    const std::size_t n = std::min(most, have);
    out.insert(out.end(), input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(n * 2));
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(n * 2));
    return n;
  }
  const int width = 2 * kHalfWidth;
  std::size_t made = 0;
  while (made < most) {
    const auto whole = static_cast<std::size_t>(at_);
    if (whole + kHalfWidth >= have) break;
    const double frac = at_ - static_cast<double>(whole);
    const double place = frac * kPhases;
    const auto phase = std::min(static_cast<int>(place), kPhases - 1);
    const auto mix = static_cast<float>(place - phase);
    const float* a = &table_[static_cast<std::size_t>(phase) * width];
    const float* b = a + width;
    const float* in = &input_[(whole + 1 - kHalfWidth) * 2];
    float left = 0, right = 0;
    for (int k = 0; k < width; ++k) {
      const float w = a[k] + (b[k] - a[k]) * mix;
      left += in[k * 2] * w;
      right += in[k * 2 + 1] * w;
    }
    out.push_back(left);
    out.push_back(right);
    ++made;
    at_ += step_;
  }
  // Drop what no later output sample needs.
  const auto keep_from = static_cast<std::size_t>(at_) + 1 - kHalfWidth;
  if (keep_from > 0 && keep_from <= have) {
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(keep_from * 2));
    at_ -= static_cast<double>(keep_from);
  }
  return made;
}

}  // namespace bd::audio
