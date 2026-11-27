// The widescreen HUD (nds::WideHud): elements found by what they are on a
// handmade screen, moved or made longer in the 3D and the 2D, left as they
// are when they cannot be matched; touches on moved elements mapped back.
// No game data: every frame here is made by hand.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gfx/screen_layout.h"
#include "nds/geometry_engine.h"
#include "nds/wide_hud.h"
#include "test.h"

namespace {

using bd::nds::GeometryEngine;
using bd::nds::HudPlan;
using bd::nds::WideHud;
using u32 = std::uint32_t;
using Frame = GeometryEngine::Frame;
using Polygon = GeometryEngine::Polygon;
using Vertex = GeometryEngine::Vertex;

constexpr int kShift = 43;  // A side at 1x.

// A flat rectangle x0..x1, y0..y1 with texture coordinates s0..s1 across
// (sixteenths of a texel) and `texture` as its texture word.
void rectangle(Frame& f, float x0, float y0, float x1, float y1, int s0, int s1, u32 texture) {
  Polygon p;
  p.first = static_cast<std::uint16_t>(f.vertices.size());
  p.size = 4;
  p.flat = true;
  p.texture = texture;
  const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
  const int ss[4] = {s0, s1, s1, s0}, ts[4] = {0, 0, 128, 128};
  for (int k = 0; k < 4; ++k) {
    Vertex v;
    v.fine_x = static_cast<std::int32_t>(std::lround(xs[k] * 16));
    v.fine_y = static_cast<std::int32_t>(std::lround(ys[k] * 16));
    v.x = v.fine_x / 16;
    v.y = v.fine_y / 16;
    v.w = 0x1000;
    v.s = static_cast<std::int16_t>(ss[k]);
    v.t = static_cast<std::int16_t>(ts[k]);
    f.vertices.push_back(v);
  }
  f.polygons.push_back(p);
}

float left_of(const Frame& f, std::size_t i) {
  float x = 1e9f;
  for (unsigned k = 0; k < f.polygons[i].size; ++k) x = std::min(x, f.vertices[f.polygons[i].first + k].fine_x / 16.0f);
  return x;
}
float right_of(const Frame& f, std::size_t i) {
  float x = -1e9f;
  for (unsigned k = 0; k < f.polygons[i].size; ++k) x = std::max(x, f.vertices[f.polygons[i].first + k].fine_x / 16.0f);
  return x;
}
// Texture s at the polygon's corner with the least (or most) x.
int s_at(const Frame& f, std::size_t i, bool right) {
  const float x = right ? right_of(f, i) : left_of(f, i);
  for (unsigned k = 0; k < f.polygons[i].size; ++k) {
    const Vertex& v = f.vertices[f.polygons[i].first + k];
    if (v.fine_x / 16.0f == x) return v.s;
  }
  return -99999;
}

constexpr u32 kFrameTexture = 0x31c00280, kButtonTexture = 0x31230c00;

// The help bar of a menu: its left end 0..8, middle 8..224, right end
// 224..232, and the B button beside it, 232..256, at the bottom.
Frame help_bar() {
  Frame f;
  rectangle(f, 0, 168, 8, 192, 0, 128, kFrameTexture);
  rectangle(f, 8, 168, 224, 192, 128, 256, kFrameTexture);
  rectangle(f, 224, 168, 232, 192, 256, 384, kFrameTexture);
  rectangle(f, 232, 160, 256, 192, 0, 384, kButtonTexture);
  return f;
}

bool same_frames(const Frame& a, const Frame& b) {
  if (a.polygons.size() != b.polygons.size()) return false;
  for (std::size_t i = 0; i < a.polygons.size(); ++i) {
    const Polygon& p = a.polygons[i];
    const Polygon& q = b.polygons[i];
    if (p.size != q.size || p.texture != q.texture || p.flat != q.flat) return false;
    for (unsigned k = 0; k < p.size; ++k)
      if (!(a.vertices[p.first + k] == b.vertices[q.first + k])) return false;
  }
  return true;
}

}  // namespace

// The help bar is made longer: its left end goes to the wide screen's left
// edge, its right end and the B button to the right side, and its middle
// goes on between with its texture at the same rate (not stretched).
TEST(wide_hud_extends_a_bar_with_its_ends_at_the_new_ends) {
  const Frame in = help_bar();
  WideHud hud;
  hud.set_shift(kShift);
  Frame out;
  hud.apply_3d(in, out);
  REQUIRE(out.polygons.size() == 4);
  CHECK(left_of(out, 0) == -kShift && right_of(out, 0) == 8 - kShift);
  CHECK(s_at(out, 0, false) == 0 && s_at(out, 0, true) == 128);
  CHECK(left_of(out, 1) == 8 - kShift && right_of(out, 1) == 224 + kShift);
  const double rate_in = 128.0 / 216.0;
  const double rate_out = (s_at(out, 1, true) - s_at(out, 1, false)) / (right_of(out, 1) - left_of(out, 1));
  CHECK(std::abs(rate_out - rate_in) < 0.01);
  CHECK(left_of(out, 2) == 224 + kShift && right_of(out, 2) == 232 + kShift);
  CHECK(s_at(out, 2, false) == 256 && s_at(out, 2, true) == 384);
  CHECK(left_of(out, 3) == 232 + kShift && right_of(out, 3) == 256 + kShift);
  // The help text with it: the area left of the right end to the left, the
  // rest to the right.
  WideHud::Screen2D screen;
  screen.text_layer = 1;
  const HudPlan& plan = hud.plan_2d(screen);
  CHECK(plan.source_x(1, 180, 10 - kShift) == 10);
  CHECK(plan.source_x(1, 180, 230 + kShift) == 230);
  CHECK(plan.source_x(1, 180, 10) == 10 + kShift);
  CHECK(plan.source_x(1, 100, 10) == 10);
  // A touch on the B button goes to it.
  CHECK(plan.touch_x(240 + kShift, 180) == 240);
}

// A box that cannot be matched (another polygon partly over its place, as
// while a box opens) stays where the game put it; so does its text.
TEST(wide_hud_leaves_an_element_it_cannot_match_centred) {
  Frame in;
  // The coins box: three pieces together 0..64 by 101..135.
  rectangle(in, 0, 101, 8, 135, 0, 128, kFrameTexture);
  rectangle(in, 8, 101, 56, 135, 128, 256, kFrameTexture);
  rectangle(in, 56, 101, 64, 135, 256, 384, kFrameTexture);
  WideHud hud;
  hud.set_shift(kShift);
  Frame out;
  hud.apply_3d(in, out);
  CHECK(left_of(out, 0) == -kShift);
  // Something partly over it.
  rectangle(in, 60, 90, 80, 120, 0, 128, kButtonTexture);
  hud.apply_3d(in, out);
  CHECK(same_frames(in, out));
  WideHud::Screen2D screen;
  screen.text_layer = 1;
  CHECK(!hud.plan_2d(screen).on);
  // A bar without its ends: as it is.
  Frame bar;
  rectangle(bar, 8, 168, 224, 192, 128, 256, kFrameTexture);
  hud.apply_3d(bar, out);
  CHECK(same_frames(bar, out));
  // A sprite partly in the field's corner: not moved.
  std::array<std::uint8_t, 0x400> sprites{};
  for (unsigned n = 0; n < 128; ++n) sprites[n * 8 + 1] = 2;  // Off.
  sprites[0] = 150;  // 32 by 32 at (0, 150): over the map icon's place.
  sprites[1] = 0;
  sprites[3] = 2u << 6;
  screen.sprites = sprites.data();
  screen.sprites_on = true;
  hud.apply_3d(Frame{}, out);
  CHECK(!hud.plan_2d(screen).on);
}

// Centred (no shift): the frame and the 2D are drawn as without the HUD.
TEST(wide_hud_centred_draws_as_without_it) {
  const Frame in = help_bar();
  WideHud hud;  // Shift 0: off.
  Frame out;
  hud.apply_3d(in, out);
  CHECK(same_frames(in, out));
  WideHud::Screen2D screen;
  screen.text_layer = 1;
  CHECK(!hud.plan_2d(screen).on);
  std::array<u32, HudPlan::kWords> words{};
  hud.plan().to_words(words.data());
  for (u32 w : words) CHECK(w == 0);
}

// A touch on a wide touch screen: on a moved element, the DS's place it came
// from; in the DS's columns, as without the HUD; at a side, nothing.
TEST(wide_hud_touch_on_a_moved_icon_maps_back) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.arrangement = Arrangement::BottomOnly;
  options.wide_screen = 1;
  const ScreenLayout layout = lay_out(1366, 768, options);
  REQUIRE(layout.bottom_wide);
  const Rect r = layout.bottom;
  // Window x of DS column `x` (from the DS screen's left edge).
  const auto window_x = [&](double x) { return r.x + (x + (kWideWidth - kScreenWidth) / 2) * r.w / kWideWidth; };
  const auto window_y = [&](double y) { return r.y + y * r.h / kScreenHeight; };
  // The map icon moved from (0, 160) to (-43, 160).
  const std::vector<TouchMove> moves = {{-kShift, 160, 32 - kShift, 192, -kShift}};
  const auto on_icon = touch_point(layout, window_x(-kShift + 5.5), window_y(170.5), moves);
  REQUIRE(on_icon.has_value());
  CHECK(on_icon->x == 5 && on_icon->y == 170);
  for (double x : {0.5, 100.5, 255.5}) {
    const auto with = touch_point(layout, window_x(x), window_y(50.5), moves);
    const auto without = touch_point(layout, window_x(x), window_y(50.5));
    REQUIRE(with.has_value() && without.has_value());
    CHECK(with->x == without->x && with->y == without->y);
  }
  CHECK(!touch_point(layout, window_x(-20.5), window_y(50.5), moves).has_value());
  // From a script, in DS pixels.
  const auto scripted = wide_touch(-kShift + 5.5, 170.5, moves);
  REQUIRE(scripted.has_value());
  CHECK(scripted->x == 5);
}
