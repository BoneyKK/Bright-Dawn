// The parts of the window that do not need a window: where the screens go,
// how their colours are passed on, the default bindings and frame pacing.
#include <filesystem>
#include <set>

#include "gfx/screen_image.h"
#include "gfx/screen_layout.h"
#include "input/bindings.h"
#include "platform/frame_clock.h"
#include "platform/paths.h"
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

TEST(layout_shows_the_touch_screen_alone) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.arrangement = Arrangement::BottomOnly;
  options.gap = 40;            // Neither the gap
  options.bottom_first = true;  // nor which is first matters.
  // Whole numbers: three times in 800 by 600, centred; the top screen has
  // no area.
  const ScreenLayout layout = lay_out(800, 600, options);
  CHECK(layout.scale == 3);
  CHECK(same(layout.bottom, 16, 12, 768, 576));
  CHECK(layout.top.w == 0 && layout.top.h == 0);
  CHECK(window_size(2, options).x == 512 && window_size(2, options).y == 384);
  // Filling the window: as large as its shape allows.
  options.scaling = Scaling::Fit;
  const ScreenLayout filled = lay_out(1000, 600, options);
  CHECK(filled.scale == 600.0 / 192);
  CHECK(same(filled.bottom, 100, 0, 800, 600));
  // Smaller than the screen: shrunk.
  options.scaling = Scaling::WholeNumbers;
  CHECK(lay_out(128, 96, options).scale == 0.5);
}

TEST(touch_maps_onto_the_touch_screen_alone) {
  using namespace bd::gfx;
  LayoutOptions options;
  options.arrangement = Arrangement::BottomOnly;
  const ScreenLayout layout = lay_out(800, 600, options);  // At (16, 12), three times the size.
  const auto at = [&](double x, double y) { return touch_point(layout, x, y); };
  CHECK(at(16, 12).has_value() && at(16, 12)->x == 0 && at(16, 12)->y == 0);
  CHECK(at(400, 300)->x == 128 && at(400, 300)->y == 96);
  CHECK(at(16 + 767.9, 12 + 575.9)->x == 255 && at(16 + 767.9, 12 + 575.9)->y == 191);
  CHECK(at(16 + 3 * 10 + 1, 12 + 3 * 20 + 2)->x == 10 && at(16 + 3 * 10 + 1, 12 + 3 * 20 + 2)->y == 20);
  // The border round it is not the touch screen, and neither is where the
  // top screen would be.
  CHECK(!at(15, 300).has_value() && !at(400, 11).has_value() && !at(784, 300).has_value() && !at(400, 588).has_value());
  // Filling a wide window.
  options.scaling = Scaling::Fit;
  const ScreenLayout wide = lay_out(1000, 600, options);
  CHECK(touch_point(wide, 100, 0)->x == 0 && touch_point(wide, 899, 599)->x == 255 && touch_point(wide, 899, 599)->y == 191);
  CHECK(!touch_point(wide, 99, 300).has_value() && !touch_point(wide, 900, 300).has_value());
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

TEST(frame_clock_makes_a_step_due_after_one_console_frame_of_real_time) {
  bd::platform::FrameClock clock;
  clock.tick(1000);  // Starts the clock; no time yet.
  CHECK(!clock.step_due());
  CHECK(clock.alpha() == 0);
  // One step is 560190 * 10^9 / 33513982 ns: 16715113 and a fraction.
  CHECK(clock.until_due() == 16715114);
  clock.tick(1000 + 16715113);
  CHECK(!clock.step_due());
  CHECK(clock.until_due() == 1);
  clock.tick(1000 + 16715114);
  CHECK(clock.step_due() && clock.until_due() == 0);
  clock.step_taken();
  CHECK(!clock.step_due());
  // Taking a step that is not there leaves the account empty.
  clock.step_taken();
  CHECK(clock.alpha() == 0);
}

TEST(frame_clock_says_how_far_into_the_next_step_real_time_is) {
  bd::platform::FrameClock clock;
  clock.tick(0);
  clock.tick(8357556);  // Half a step, to within a nanosecond.
  CHECK(clock.alpha() > 0.4999999 && clock.alpha() < 0.5);
  // Time that runs backwards adds nothing.
  clock.tick(100);
  CHECK(clock.alpha() > 0.4999999 && clock.alpha() < 0.5);
}

TEST(frame_clock_keeps_the_console_rate_without_drift) {
  // Ticks every millisecond for 100 seconds, taking every step that is due:
  // exactly 100 * 10^9 * 33513982 / (560190 * 10^9) steps, rounded down.
  bd::platform::FrameClock clock;
  std::uint64_t steps = 0;
  for (std::uint64_t ms = 0; ms <= 100000; ++ms) {
    clock.tick(ms * 1000000);
    while (clock.step_due()) {
      clock.step_taken();
      ++steps;
    }
  }
  CHECK(steps == 5982);
  CHECK(clock.drops() == 0);
}

TEST(frame_clock_shows_a_faster_screen_more_pictures_than_steps) {
  // A screen refreshed 120 times a second: two pictures a step, give or
  // take the difference between 120 and twice 59.83.
  bd::platform::FrameClock clock;
  std::uint64_t steps = 0, pictures = 0;
  for (std::uint64_t refresh = 0; refresh <= 1200; ++refresh) {
    clock.tick(refresh * 1'000'000'000ull / 120);
    while (clock.step_due()) {
      clock.step_taken();
      ++steps;
    }
    ++pictures;
  }
  CHECK(steps == 598);  // Ten seconds: 59.83 a second, rounded down.
  CHECK(pictures == 1201);
  CHECK(clock.drops() == 0);
}

TEST(frame_clock_drops_time_it_cannot_catch_up_on) {
  bd::platform::FrameClock clock;
  clock.tick(0);
  // Four steps' worth and a little more are kept, to be caught up on.
  clock.tick(4 * 16715113);
  CHECK(clock.drops() == 0);
  // A second is far more: one step is left, and the rest dropped.
  clock.tick(4 * 16715113 + 1'000'000'000ull);
  CHECK(clock.drops() == 1);
  CHECK(clock.step_due());
  clock.step_taken();
  CHECK(!clock.step_due() && clock.alpha() == 0);
  // A very long pause is no different, and does not overflow.
  clock.tick(~std::uint64_t{0} / 2);
  CHECK(clock.drops() == 2 && clock.step_due());
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

TEST(frame_clock_shows_frames_run_on_time) {
  bd::platform::FrameClock clock;
  clock.tick(0);
  // Each frame is run in a millisecond: shown, and the next not yet due.
  CHECK(clock.frame_done(1000000));
  CHECK(!clock.step_due());
}

TEST(frame_clock_catches_up_after_a_hitch_before_showing_again) {
  bd::platform::FrameClock clock;
  clock.tick(0);
  // Three steps of real time pass during one frame; then frames are run
  // quickly. The slow frame is shown (nothing was due when it began), the two
  // steps owed are then run unshown, and the one after is shown.
  std::uint64_t now = 3 * 16715114;
  CHECK(clock.frame_done(now));
  CHECK(!clock.frame_done(now += 1000));
  CHECK(!clock.frame_done(now += 1000));
  CHECK(clock.frame_done(now += 1000));
  CHECK(!clock.step_due());
  CHECK(clock.drops() == 0);
}

TEST(frame_clock_shows_every_frame_when_the_computer_cannot_keep_up) {
  // Each frame takes three steps of real time: time is dropped, and from
  // then on every frame is shown.
  bd::platform::FrameClock clock;
  clock.tick(0);
  std::uint64_t now = 0, shown = 0;
  for (int frame = 0; frame < 100; ++frame) shown += clock.frame_done(now += 3 * 16715114) ? 1 : 0;
  CHECK(shown == 100);
  CHECK(clock.drops() > 0);
}

TEST(frame_clock_shows_a_frame_after_four_unshown_in_a_row) {
  // Each frame takes a step and a quarter: the game falls behind by a quarter
  // step a frame. Once a step is due when a frame begins, four frames go
  // unshown, then one is shown regardless.
  bd::platform::FrameClock clock;
  clock.tick(0);
  std::uint64_t now = 0;
  for (int frame = 0; frame < 4; ++frame) CHECK(clock.frame_done(now += 20893892));
  for (int frame = 0; frame < 4; ++frame) CHECK(!clock.frame_done(now += 20893892));
  CHECK(clock.frame_done(now += 20893892));
  CHECK(clock.drops() == 0);
}

TEST(frame_clock_shows_every_frame_with_vsync_at_60_hz) {
  // A frame is run in 8 ms, then the picture waits for the screen, refreshed
  // 60 times a second, and is shown again until the next step is due (as
  // GameScreen::frame does). The console's 59.83 frames a second all fit:
  // every frame is shown. Counting the run's time before deciding hid every
  // second one.
  bd::platform::FrameClock clock;
  clock.tick(0);
  std::uint64_t now = 0, refresh = 0, shown = 0;
  constexpr std::uint64_t kRefresh = 16666667;
  for (int frame = 0; frame < 600; ++frame) {
    now += 8'000'000;
    if (!clock.frame_done(now)) continue;
    ++shown;
    do {
      while (refresh <= now) refresh += kRefresh;
      now = refresh;
      clock.tick(now);
    } while (!clock.step_due());
  }
  CHECK(shown == 600);
  CHECK(clock.drops() == 0);
}

TEST(frame_clock_owes_nothing_for_a_pause) {
  bd::platform::FrameClock clock;
  clock.tick(0);
  clock.tick(8357556);  // Half a step,
  clock.restart(5'000'000'000ull);  // then a pause of five seconds.
  CHECK(!clock.step_due() && clock.alpha() == 0 && clock.until_due() == 16715114);
  CHECK(clock.drops() == 0);
  clock.tick(5'000'000'000ull + 16715114);
  CHECK(clock.step_due());
  // A frame after the pause is shown.
  CHECK(clock.frame_done(5'000'000'000ull + 16715114 + 1));
}

TEST(screens_are_enlarged_to_the_whole_multiple_at_or_above_the_scale) {
  using bd::gfx::prescale_factor;
  // Smoothing then only shrinks, by less than half.
  CHECK(prescale_factor(2.0, 8192) == 2);
  CHECK(prescale_factor(2.0000001, 8192) == 2);  // The layout's arithmetic, a hair over.
  CHECK(prescale_factor(1.9999999, 8192) == 2);
  CHECK(prescale_factor(2.3, 8192) == 3);
  CHECK(prescale_factor(4.97, 8192) == 5);
  CHECK(prescale_factor(1.0, 8192) == 1 && prescale_factor(0.5, 8192) == 1 && prescale_factor(0, 8192) == 1);
  // No larger than a texture may be: 2048 / 256 = 8.
  CHECK(prescale_factor(11.2, 2048) == 8);
  CHECK(prescale_factor(3.5, 100) == 1);
}

TEST(the_program_knows_its_own_file) {
  const auto path = bd::platform::executable_path();
  REQUIRE(path.ok());
  CHECK(std::filesystem::is_regular_file(path.value()));
  CHECK(path.value().stem().string() == "bd_tests");
}
