#include "app/backdrop.h"

#include <algorithm>
#include <vector>

#include "core/log.h"
#include "gfx/screen_layout.h"

namespace bd::app {

void Backdrop::set(assets::Picture picture) {
  release();
  picture_ = std::move(picture);
  failed_ = false;
}

void Backdrop::clear() {
  release();
  picture_.reset();
}

void Backdrop::release() {
  if (texture_ != nullptr) SDL_DestroyTexture(texture_);
  texture_ = nullptr;
  factor_ = 0;
}

bool Backdrop::prepare(SDL_Renderer* renderer, int factor) {
  if (texture_ != nullptr && factor_ == factor) return true;
  release();
  const assets::Picture& p = *picture_;
  const int w = p.width * factor, h = p.height * factor;
  std::vector<std::uint8_t> big(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* from = p.rgba.data() + static_cast<std::size_t>(y / factor) * p.width * 4;
    std::uint8_t* to = big.data() + static_cast<std::size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) std::copy_n(from + (x / factor) * 4, 4, to + x * 4);
  }
  texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (texture_ == nullptr || !SDL_UpdateTexture(texture_, nullptr, big.data(), w * 4)) {
    log::warning("launcher", std::string("the backdrop cannot be made: ") + SDL_GetError());
    release();
    failed_ = true;
    return false;
  }
  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
  factor_ = factor;
  return true;
}

void Backdrop::draw(SDL_Renderer* renderer) {
  if (!picture_ || failed_) return;
  int out_w = 0, out_h = 0;
  if (!SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h) || out_w <= 0 || out_h <= 0) return;
  const double scale = std::max(static_cast<double>(out_w) / picture_->width,
                                static_cast<double>(out_h) / picture_->height);
  int largest = 4096;
  if (SDL_PropertiesID props = SDL_GetRendererProperties(renderer))
    largest = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
  // Small windows need no more than the picture as it is.
  const int factor = std::max(1, gfx::prescale_factor(scale, largest));
  if (!prepare(renderer, factor)) return;
  const float w = static_cast<float>(picture_->width * scale), h = static_cast<float>(picture_->height * scale);
  const SDL_FRect to{(out_w - w) / 2.0f, (out_h - h) / 2.0f, w, h};
  SDL_RenderTexture(renderer, texture_, nullptr, &to);
}

}  // namespace bd::app
