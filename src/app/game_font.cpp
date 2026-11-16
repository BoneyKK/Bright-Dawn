#include "app/game_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

namespace bd::app {
namespace {

const GameFontSource& source_of(const ImFontConfig* src) {
  return *static_cast<const GameFontSource*>(src->FontLoaderData);
}

bool font_src_init(ImFontAtlas*, ImFontConfig* src) {
  src->FontLoaderData = src->FontData;
  return src->FontLoaderData != nullptr;
}

void font_src_destroy(ImFontAtlas*, ImFontConfig* src) { src->FontLoaderData = nullptr; }

bool font_src_contains_glyph(ImFontAtlas*, ImFontConfig* src, ImWchar codepoint) {
  return source_of(src).font->find(static_cast<char32_t>(codepoint)) != nullptr;
}

// Where a glyph is drawn in its line, and how far the line goes on after
// it, in the font's pixels. The font's punctuation and symbols are full
// width, as Japanese text sets them: centred in a cell as wide as the font.
// In Bright Dawn's Latin text they are set close, as the letters are.
struct Placement {
  int left = 0;
  int advance = 0;
};

Placement placement(const assets::Font& font, const assets::Glyph& glyph) {
  Placement p{glyph.left, glyph.advance};
  if (glyph.width > 0 && glyph.advance >= font.cell_width() - 1) {
    p.left = std::min(glyph.left, 1);
    p.advance = std::min(glyph.advance, p.left + glyph.width);
  }
  // One pixel between characters, as the game leaves.
  p.advance += 1;
  return p;
}

bool font_baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
  if (!src->MergeMode) {
    const assets::Font& font = *source_of(src).font;
    const int k = game_font_scale(font, baked->Size);
    baked->Ascent = static_cast<float>(font.baseline() * k);
    baked->Descent = -static_cast<float>((font.line_height() - font.baseline()) * k);
  }
  return true;
}

// A glyph's pixels, outlined or not, as white ink (black outline) on clear,
// each font pixel `k` by `k`. The result is (columns * k) by (rows * k).
std::vector<std::uint8_t> render(const assets::Font& font, const assets::Glyph& glyph, bool outline, int k,
                                 int columns, int rows) {
  const int border = outline ? 1 : 0;
  const auto ink = [&](int x, int y) {  // In the glyph's own cell.
    if (x < 0 || y < 0 || x >= glyph.width || x >= font.cell_width() || y >= font.cell_height()) return false;
    return glyph.pixels[static_cast<std::size_t>(y * font.cell_width() + x)] != 0;
  };
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(columns * k) * static_cast<std::size_t>(rows * k) * 4, 0);
  const int pitch = columns * k * 4;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < columns; ++x) {
      const int gx = x - border, gy = y - border;
      std::uint8_t value = 0;
      bool lit = false;
      if (ink(gx, gy)) {
        // Partial ink in fonts of more than one bit a pixel: as coverage.
        value = static_cast<std::uint8_t>(glyph.pixels[static_cast<std::size_t>(gy * font.cell_width() + gx)] * 255 /
                                          font.max_value());
        lit = true;
      }
      std::uint8_t r = 255, a = value;
      if (!lit && outline) {
        bool near = false;
        for (int dy = -1; dy <= 1 && !near; ++dy)
          for (int dx = -1; dx <= 1 && !near; ++dx) near = ink(gx + dx, gy + dy);
        if (!near) continue;
        r = 0;
        a = 255;
      }
      for (int sy = 0; sy < k; ++sy) {
        std::uint8_t* row = rgba.data() + static_cast<std::size_t>((y * k + sy) * pitch);
        for (int sx = 0; sx < k; ++sx) {
          std::uint8_t* p = row + static_cast<std::size_t>((x * k + sx) * 4);
          p[0] = p[1] = p[2] = r;
          p[3] = a;
        }
      }
    }
  }
  return rgba;
}

bool font_baked_load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar codepoint,
                           ImFontGlyph* out_glyph, float* out_advance_x) {
  const GameFontSource& source = source_of(src);
  const assets::Font& font = *source.font;
  const assets::Glyph* glyph = font.find(static_cast<char32_t>(codepoint));
  if (glyph == nullptr) return false;
  const int k = game_font_scale(font, baked->Size);
  const Placement place = placement(font, *glyph);
  const float advance = static_cast<float>(place.advance * k);
  if (out_advance_x != nullptr) {
    *out_advance_x = advance;
    return true;
  }
  out_glyph->Codepoint = codepoint;
  out_glyph->AdvanceX = advance;
  const bool any_ink = std::any_of(glyph->pixels.begin(), glyph->pixels.end(), [](std::uint8_t v) { return v != 0; });
  const int used = std::min(glyph->width, font.cell_width());
  if (!any_ink || used <= 0) return true;  // A space: an advance and nothing to draw.

  const int border = source.outline ? 1 : 0;
  const int columns = used + 2 * border, rows = font.cell_height() + 2 * border;
  const int w = columns * k, h = rows * k;
  const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, w, h);
  if (pack_id == ImFontAtlasRectId_Invalid) return false;
  ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack_id);
  const std::vector<std::uint8_t> pixels = render(font, *glyph, source.outline, k, columns, rows);
  out_glyph->X0 = static_cast<float>((place.left - border) * k);
  out_glyph->Y0 = static_cast<float>(-border * k);
  out_glyph->X1 = out_glyph->X0 + static_cast<float>(w);
  out_glyph->Y1 = out_glyph->Y0 + static_cast<float>(h);
  out_glyph->Visible = true;
  out_glyph->PackId = pack_id;
  ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, pixels.data(), ImTextureFormat_RGBA32, w * 4);
  return true;
}

}  // namespace

int game_font_scale(const assets::Font& font, float size) {
  const int line = std::max(1, font.line_height());
  return std::max(1, static_cast<int>(std::lround(size / static_cast<float>(line))));
}

const ImFontLoader* game_font_loader() {
  static const ImFontLoader loader = [] {
    ImFontLoader l;
    l.Name = "Bright Dawn: the game's font";
    l.FontSrcInit = font_src_init;
    l.FontSrcDestroy = font_src_destroy;
    l.FontSrcContainsGlyph = font_src_contains_glyph;
    l.FontBakedInit = font_baked_init;
    l.FontBakedLoadGlyph = font_baked_load_glyph;
    return l;
  }();
  return &loader;
}

}  // namespace bd::app
