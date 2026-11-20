#include "app/menu_skin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "core/log.h"

namespace bd::app {
namespace {

// Each picture is enlarged this many times once; the renderer's linear
// filtering then softens only the edges between its pixels.
constexpr int kEnlarge = 4;

ImTextureRef ref(SDL_Texture* texture) { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))); }

// ImGui's coordinates for each of the renderer's pixels.
float pixels_per_unit() {
  const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
  return scale.x > 0 ? scale.x : 1.0f;
}

}  // namespace

void SkinPicture::set(assets::Picture picture) {
  release();
  picture_ = std::move(picture);
  failed_ = false;
}

void SkinPicture::clear() {
  release();
  picture_.reset();
}

void SkinPicture::release() {
  if (texture_ != nullptr) SDL_DestroyTexture(texture_);
  texture_ = nullptr;
}

SDL_Texture* SkinPicture::texture(SDL_Renderer* renderer) {
  if (texture_ != nullptr || !picture_ || failed_) return texture_;
  const assets::Picture& p = *picture_;
  const int w = p.width * kEnlarge, h = p.height * kEnlarge;
  std::vector<std::uint8_t> big(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* from = p.rgba.data() + static_cast<std::size_t>(y / kEnlarge) * static_cast<std::size_t>(p.width) * 4;
    std::uint8_t* to = big.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
    for (int x = 0; x < w; ++x) std::copy_n(from + (x / kEnlarge) * 4, 4, to + static_cast<std::ptrdiff_t>(x) * 4);
  }
  texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (texture_ == nullptr || !SDL_UpdateTexture(texture_, nullptr, big.data(), w * 4)) {
    log::warning("launcher", std::string("a picture of the game's menus cannot be made: ") + SDL_GetError());
    release();
    failed_ = true;
    return nullptr;
  }
  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
  return texture_;
}

void MenuSkin::read(const rom::Rom& rom) {
  clear();
  std::vector<std::string> missing;
  const auto picture = [&](SkinPicture& into, std::string_view path, int width, int height, bool clear_zero) {
    Result<assets::Picture> p = width == assets::kScreenPictureWidth && height == assets::kScreenPictureHeight
                                    ? assets::read_screen_picture(rom, path)
                                    : assets::read_indexed_picture(rom, path, width, height, clear_zero);
    if (!p) {
      missing.push_back(p.error().message);
      return;
    }
    into.set(std::move(p).value());
  };
  picture(background_, skin::kBackground, 256, 192, false);
  picture(strip_, skin::kStrip, 232, 32, true);
  picture(ribbon_left_, skin::kRibbonLeft, 40, 20, true);
  picture(ribbon_middle_, skin::kRibbonMiddle, 16, 20, true);
  picture(ribbon_right_, skin::kRibbonRight, 40, 20, true);
  picture(paper_, skin::kJournal, 256, 192, false);
  picture(save_page_, skin::kSavePage, 256, 192, false);
  Result<assets::Font> font = assets::read_font(rom, skin::kFont);
  if (font) font_ = std::make_shared<const assets::Font>(std::move(font).value());
  else missing.push_back(font.error().message);
  if (!missing.empty()) {
    for (const std::string& m : missing) log::warning("launcher", "the game's menus are not as expected: " + m);
    log::warning("launcher", "the launcher keeps its own look");
    clear();
    return;
  }
  ready_ = true;
  log::info("launcher", "the launcher is dressed in the game's menus, from the ROM");
}

void MenuSkin::clear() {
  ready_ = false;
  for (SkinPicture* p : {&background_, &strip_, &ribbon_left_, &ribbon_middle_, &ribbon_right_, &paper_, &save_page_}) p->clear();
  font_.reset();
}

void MenuSkin::release() {
  for (SkinPicture* p : {&background_, &strip_, &ribbon_left_, &ribbon_middle_, &ribbon_right_, &paper_, &save_page_}) p->release();
}

namespace {

// The parts of the background that are stretched when the window is wider
// than the picture (columns) or taller (rows): only wood and sky are in
// them, no gear. The cuts between the parts are where the picture is split.
struct Span {
  float from, to;
};
constexpr Span kWideParts[] = {{50, 82}, {174, 206}};
constexpr Span kTallParts[] = {{52, 140}};
constexpr float kColumnCuts[] = {0, 50, 82, 174, 206, 256};
constexpr float kRowCuts[] = {0, 52, 140, 192};

template <std::size_t N>
float stretched(float p, const Span (&parts)[N], float scale, float extra) {
  float total = 0, before = 0;
  for (const Span& part : parts) {
    total += part.to - part.from;
    before += std::clamp(p - part.from, 0.0f, part.to - part.from);
  }
  return p * scale + (total > 0 ? extra * before / total : 0.0f);
}

}  // namespace

bool MenuSkin::fit(SDL_Renderer* renderer, Fit& fit) const {
  int out_w = 0, out_h = 0;
  if (!ready_ || !SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h) || out_w <= 0 || out_h <= 0) return false;
  const float w = static_cast<float>(out_w), h = static_cast<float>(out_h);
  fit.scale = std::min(w / 256.0f, h / 192.0f);
  fit.extra_width = std::max(0.0f, w - 256.0f * fit.scale);
  fit.extra_height = std::max(0.0f, h - 192.0f * fit.scale);
  return true;
}

ImVec2 MenuSkin::place(const Fit& fit, float x, float y) {
  return ImVec2(stretched(x, kWideParts, fit.scale, fit.extra_width),
                stretched(y, kTallParts, fit.scale, fit.extra_height));
}

void MenuSkin::draw_whole(SDL_Renderer* renderer, SkinPicture& picture) {
  Fit f;
  if (!fit(renderer, f)) return;
  SDL_Texture* texture = picture.texture(renderer);
  if (texture == nullptr) return;
  const float e = static_cast<float>(kEnlarge);
  for (std::size_t row = 0; row + 1 < std::size(kRowCuts); ++row) {
    for (std::size_t column = 0; column + 1 < std::size(kColumnCuts); ++column) {
      const float x0 = kColumnCuts[column], x1 = kColumnCuts[column + 1];
      const float y0 = kRowCuts[row], y1 = kRowCuts[row + 1];
      const SDL_FRect from{x0 * e, y0 * e, (x1 - x0) * e, (y1 - y0) * e};
      const ImVec2 a = place(f, x0, y0), b = place(f, x1, y1);
      const SDL_FRect to{a.x, a.y, b.x - a.x, b.y - a.y};
      SDL_RenderTexture(renderer, texture, &from, &to);
    }
  }
}

ImVec2 MenuSkin::at(SDL_Renderer* renderer, float x, float y) const {
  Fit f;
  const ImGuiViewport* view = ImGui::GetMainViewport();
  if (!fit(renderer, f)) return view->Pos;
  const float unit = pixels_per_unit();
  const ImVec2 p = place(f, x, y);
  return ImVec2(view->Pos.x + p.x / unit, view->Pos.y + p.y / unit);
}

ImRect MenuSkin::area(SDL_Renderer* renderer, float left, float top, float right, float bottom) const {
  return ImRect(at(renderer, left, top), at(renderer, right, bottom));
}

float MenuSkin::picture_scale(SDL_Renderer* renderer) const {
  Fit f;
  return fit(renderer, f) ? f.scale / pixels_per_unit() : 1.0f;
}

ImRect MenuSkin::board(SDL_Renderer* renderer) const {
  return area(renderer, skin::kBoardLeft, skin::kBoardTop, skin::kBoardRight, skin::kBoardBottom);
}

void MenuSkin::draw_strip(ImDrawList* list, SDL_Renderer* renderer, ImVec2 min, ImVec2 max) {
  SDL_Texture* texture = strip_.texture(renderer);
  if (texture == nullptr || max.x <= min.x || max.y <= min.y) return;
  // The ends as they are, the middle stretched: the torn edges keep their
  // shape at any length. The ends are as wide as the strip is tall, scaled.
  const float scale = (max.y - min.y) / 32.0f;
  const float end = std::min(12.0f * scale, (max.x - min.x) / 2.0f);
  const float u_end = 12.0f / 232.0f;
  list->AddImage(ref(texture), min, ImVec2(min.x + end, max.y), ImVec2(0, 0), ImVec2(u_end, 1));
  list->AddImage(ref(texture), ImVec2(min.x + end, min.y), ImVec2(max.x - end, max.y), ImVec2(u_end, 0),
                 ImVec2(1 - u_end, 1));
  list->AddImage(ref(texture), ImVec2(max.x - end, min.y), max, ImVec2(1 - u_end, 0), ImVec2(1, 1));
}

void MenuSkin::draw_paper(ImDrawList* list, SDL_Renderer* renderer, ImVec2 min, ImVec2 max) {
  SDL_Texture* texture = paper_.texture(renderer);
  if (texture == nullptr) return;
  list->AddImage(ref(texture), min, max, ImVec2(skin::kPaperLeft / 256.0f, skin::kPaperTop / 192.0f),
                 ImVec2(skin::kPaperRight / 256.0f, skin::kPaperBottom / 192.0f));
}

ImRect MenuSkin::draw_ribbon(ImDrawList* list, SDL_Renderer* renderer, ImVec2 centre, float height, float text_width) {
  const float scale = height / 20.0f;
  const float end = 40.0f * scale;
  // The title lies on the middle and the inner part of each end.
  const float inner = 22.0f * scale;
  const float middle = std::max(16.0f * scale, text_width - 2 * inner + 8.0f * scale);
  const float left = centre.x - middle / 2 - end, top = centre.y - height / 2;
  SDL_Texture* l = ribbon_left_.texture(renderer);
  SDL_Texture* m = ribbon_middle_.texture(renderer);
  SDL_Texture* r = ribbon_right_.texture(renderer);
  if (l != nullptr && m != nullptr && r != nullptr) {
    list->AddImage(ref(l), ImVec2(left, top), ImVec2(left + end, top + height));
    list->AddImage(ref(m), ImVec2(left + end, top), ImVec2(left + end + middle, top + height));
    list->AddImage(ref(r), ImVec2(left + end + middle, top), ImVec2(left + 2 * end + middle, top + height));
  }
  return ImRect(left + end - inner, top, left + end + middle + inner, top + height);
}

}  // namespace bd::app
