// The parts of the window that do not need a window: where the screens go,
// how their colours are passed on, the default bindings and frame pacing.
#include <set>

#include "gfx/screen_image.h"
#include "gfx/screen_layout.h"
#include "input/bindings.h"
#include "platform/frame_pacer.h"
#include "test.h"

namespace {

bool same(const bd::gfx::Rect& r, double x, double y, double w, double h) {
  return r.x == x && r.y == y && r.w == w && r.h == h;
}

}  // namespace

TEST(layout_stacks_the_screens_at_the_largest_whole_scale) {
  using namespace bd::gfx;
  const ScreenLayout exact = lay_out(256, 384, {});
  CHECK(exact.scale == 1);
  CHECK(same(exact.top, 0, 0, 256, 192));
  CHECK(same(exact.bottom, 0, 192, 256, 192));
  // 800 by 600 fits 1.5625 times: once, centred.
  const ScreenLayout centred = lay_out(800, 600, {});
  CHECK(centred.scale == 1);
  CHECK(same(centred.top, 272, 108, 256, 192));
  CHECK(same(centred.bottom, 272, 300, 256, 192));
}

TEST(layout_can_fill_the_window_as_far_as_the_shape_allows) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.scaling = Scaling::Fit;
  const ScreenLayout layout = lay_out(800, 600, options);
  CHECK(layout.scale == 1.5625);
  CHECK(same(layout.top, 200, 0, 400, 300));
  CHECK(same(layout.bottom, 200, 300, 400, 300));
}

TEST(layout_puts_the_screens_side_by_side_with_a_scaled_gap) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.arrangement = Arrangement::SideBySide;
  options.gap = 16;
  const ScreenLayout layout = lay_out(1056, 384, options);
  CHECK(layout.scale == 2);
  CHECK(same(layout.top, 0, 0, 512, 384));
  CHECK(same(layout.bottom, 544, 0, 512, 384));
  CHECK(window_size(2, options).x == 1056 && window_size(2, options).y == 384);
}

TEST(layout_can_put_the_touch_screen_first) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.bottom_first = true;
  const ScreenLayout layout = lay_out(256, 384, options);
  CHECK(same(layout.bottom, 0, 0, 256, 192));
  CHECK(same(layout.top, 0, 192, 256, 192));
}

TEST(layout_shrinks_the_screens_when_not_even_one_whole_scale_fits) {
  using namespace bd::gfx;
  const ScreenLayout small = lay_out(128, 192, {});
  CHECK(small.scale == 0.5);
  CHECK(same(small.top, 0, 0, 128, 96));
  CHECK(same(small.bottom, 0, 96, 128, 96));
  const ScreenLayout none = lay_out(0, 384, {});
  CHECK(none.scale == 0 && none.bottom.w == 0);
  CHECK(!touch_point(none, 0, 0).has_value());
}

TEST(layout_gives_the_window_size_for_a_scale) {
  using namespace bd::gfx;
  CHECK(window_size(3, {}).x == 768 && window_size(3, {}).y == 1152);
  // Less than one is taken as one.
  CHECK(window_size(0, {}).x == 256 && window_size(0, {}).y == 384);
}

TEST(layout_finds_the_touch_screen_pixel_under_a_point) {
  using namespace bd::gfx;
  const ScreenLayout one = lay_out(256, 384, {});
  const auto corner = touch_point(one, 10, 192);
  CHECK(corner.has_value() && corner->x == 10 && corner->y == 0);
  const auto last = touch_point(one, 255.9, 383.9);
  CHECK(last.has_value() && last->x == 255 && last->y == 191);
  // The top screen, and outside both, are not the touch screen.
  CHECK(!touch_point(one, 10, 191.9).has_value());
  CHECK(!touch_point(one, 256, 200).has_value());
  CHECK(!touch_point(one, 10, 384).has_value());
  // Twice the size: two window pixels to a touch screen pixel.
  const ScreenLayout two = lay_out(512, 768, {});
  const auto middle = touch_point(two, 100, 384 + 50);
  CHECK(middle.has_value() && middle->x == 50 && middle->y == 25);
  const auto end = touch_point(two, 511, 767);
  CHECK(end.has_value() && end->x == 255 && end->y == 191);
}

TEST(screen_image_makes_six_bits_eight) {
  using bd::gfx::eight_bits;
  CHECK(eight_bits(0) == 0);
  CHECK(eight_bits(63) == 255);
  CHECK(eight_bits(31) == 125);
  CHECK(eight_bits(32) == 130);
  // Red 63, green 0, blue 31, with something above the colour.
  const std::uint32_t pixels[2] = {63u | 31u << 12 | 0x1Fu << 18, 1u << 6};
  std::uint8_t out[8] = {};
  bd::gfx::to_rgba_bytes(pixels, 2, out);
  CHECK(out[0] == 255 && out[1] == 0 && out[2] == 125 && out[3] == 255);
  CHECK(out[4] == 0 && out[5] == 4 && out[6] == 0 && out[7] == 255);
}

TEST(default_bindings_press_every_button_once) {
  using namespace bd::input;
  for (const auto* bindings : {&default_keyboard_bindings(), &default_controller_bindings()}) {
    std::set<unsigned> buttons;
    std::set<std::string> controls;
    for (const Binding& b : *bindings) {
      buttons.insert(static_cast<unsigned>(b.button));
      controls.insert(b.control);
    }
    CHECK(bindings->size() == kDsButtonCount);
    CHECK(buttons.size() == kDsButtonCount);
    CHECK(controls.size() == kDsButtonCount);
  }
  CHECK(std::string(name(DsButton::Select)) == "Select");
  CHECK(std::string(name(DsButton::Y)) == "Y");
  CHECK(bit(DsButton::Start) == 1u << 3);
}

TEST(stick_presses_the_directions_it_is_pushed_past_the_threshold) {
  using namespace bd::input;
  CHECK(directions_from_stick(0, 0) == 0);
  CHECK(directions_from_stick(16384, 0) == bit(DsButton::Right));
  CHECK(directions_from_stick(16383, -16383) == 0);
  CHECK(directions_from_stick(-16384, -32768) == (bit(DsButton::Left) | bit(DsButton::Up)));
  CHECK(directions_from_stick(0, 32767) == bit(DsButton::Down));
}

TEST(frame_pacer_keeps_the_console_rate_without_drift) {
  bd::platform::FramePacer pacer;
  // One frame is 560190 * 10^9 / 33513982 ns: 16715113 and a fraction.
  std::uint64_t next = pacer.frame_shown(0);
  CHECK(next == 16715113);
  for (int frame = 2; frame <= 100000; ++frame) next = pacer.frame_shown(next);
  CHECK(next == 1671511311308ull);
  CHECK(pacer.restarts() == 0);
}

TEST(frame_pacer_starts_again_when_far_behind) {
  bd::platform::FramePacer pacer;
  std::uint64_t next = pacer.frame_shown(1000);
  CHECK(next == 1000 + 16715113);
  // Four frames late is still caught up on.
  next = pacer.frame_shown(next + 4 * 16715113);
  CHECK(pacer.restarts() == 0);
  CHECK(next == 1000 + 33430226);  // Two frames on from the start, the fractions included.
  // More than that, and the count starts from now.
  const std::uint64_t late = next + 4 * 16715113 + 1;
  CHECK(pacer.frame_shown(late) == late + 16715113);
  CHECK(pacer.restarts() == 1);
}

TEST(buttons_are_found_by_name_in_any_case) {
  using namespace bd::input;
  CHECK(button_named("start") == DsButton::Start);
  CHECK(button_named("SELECT") == DsButton::Select);
  CHECK(button_named("y") == DsButton::Y);
  CHECK(!button_named("Starts").has_value());
  CHECK(!button_named("").has_value());
}

TEST(scripted_presses_are_read_and_held_over_their_frames) {
  using namespace bd::input;
  const auto range = parse_scripted_press("Start@300-310");
  REQUIRE(range.has_value());
  CHECK(range->button == DsButton::Start && range->first == 300 && range->last == 310);
  const auto one = parse_scripted_press("a@42");
  REQUIRE(one.has_value());
  CHECK(one->button == DsButton::A && one->first == 42 && one->last == 42);
  for (const char* bad : {"Start", "Start@", "Start@0", "Start@10-9", "Start@5-", "Start@x", "Turbo@5", "Start@5-6-7"})
    CHECK(!parse_scripted_press(bad).has_value());
  const std::vector<ScriptedPress> script = {*range, *one};
  CHECK(scripted_buttons(script, 299) == 0);
  CHECK(scripted_buttons(script, 300) == bit(DsButton::Start));
  CHECK(scripted_buttons(script, 310) == bit(DsButton::Start));
  CHECK(scripted_buttons(script, 311) == 0);
  CHECK(scripted_buttons(script, 42) == bit(DsButton::A));
}

TEST(scripted_touches_are_read_and_found_by_frame) {
  using namespace bd::input;
  const auto touch = parse_scripted_touch("128,96@600-610");
  REQUIRE(touch.has_value());
  CHECK(touch->x == 128 && touch->y == 96 && touch->first == 600 && touch->last == 610);
  const auto corner = parse_scripted_touch("255,191@1");
  REQUIRE(corner.has_value());
  CHECK(corner->x == 255 && corner->y == 191 && corner->first == 1 && corner->last == 1);
  for (const char* bad : {"128,96", "128@5", "256,0@5", "0,192@5", "-1,0@5", "1,,2@5", "1,2@0", "a,2@5", ",2@5"})
    CHECK(!parse_scripted_touch(bad).has_value());
  const std::vector<ScriptedTouch> script = {*touch, *corner};
  CHECK(scripted_touch(script, 599) == nullptr);
  CHECK(scripted_touch(script, 600) == &script[0]);
  CHECK(scripted_touch(script, 1) == &script[1]);
}
