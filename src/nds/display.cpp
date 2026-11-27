#include "nds/display.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "debug/frame_log.h"

namespace bd::nds {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Six bits a part made five, with the bit that says "a pixel is here".
u32 five_bits_each(u32 colour) {
  return 0x8000 | ((colour >> 1) & 0x1F) | (((colour >> 7) & 0x1F) << 5) | (((colour >> 13) & 0x1F) << 10);
}

// Five bits a part made six, as the display engines read video memory.
u32 six_bits_each(u32 colour) { return ((colour & 0x1F) << 1) | ((colour & 0x3E0) << 2) | ((colour & 0x7C00) << 3); }

// `pixel` of the processor's 3D picture (six bits a part, alpha from bit
// 18) put into `below`, a pixel of the main engine's picture mixed as if the
// 3D layer were empty, as `mark` (DisplayEngine::kMark3D and the rest)
// says: as the graphics hardware puts its scene in (app/shaders/compose.frag).
u32 put_in_3d(u32 below, u32 mark, u32 pixel) {
  const u32 alpha = (pixel >> 18) & 0x3F;
  if (DisplayEngine::is_3d_below(mark) && alpha != 0) {
    // `below` is the layer on top, blended with the 3D pixel below it.
    const u32 weight_top = (mark >> DisplayEngine::kMarkFactorShift) & 31;
    const u32 weight_3d = (mark >> DisplayEngine::kMarkBelowWeightShift) & 31;
    u32 result = 0;
    for (unsigned shift = 0; shift < 18; shift += 6)
      result |= std::min<u32>((((below >> shift) & 0x3F) * weight_top + ((pixel >> shift) & 0x3F) * weight_3d) / 16, 63)
                << shift;
    return result;
  }
  if ((mark & DisplayEngine::kMark3D) == 0 || alpha == 0) return below;
  const u32 effect = (mark >> DisplayEngine::kMarkEffectShift) & 3;
  const u32 factor = (mark >> DisplayEngine::kMarkFactorShift) & 31;
  u32 result = 0;
  for (unsigned shift = 0; shift < 18; shift += 6) {
    u32 part = (pixel >> shift) & 0x3F;
    if (effect == 2) part += (63 - part) * factor / 16;
    if (effect == 3) part -= part * factor / 16;
    // Blended by the 3D pixel's own alpha, as DisplayEngine::blend() does.
    if (effect == 1 && alpha != 63) part = std::min<u32>((part * (alpha + 1) + ((below >> shift) & 0x3F) * (63 - alpha)) / 64, 63);
    result |= part << shift;
  }
  return result;
}

// The sizes a capture can have, across and down.
constexpr unsigned kCaptureSizes[4][2] = {{128, 128}, {256, 64}, {256, 128}, {256, 192}};

}  // namespace

Display::Display(VideoMemory& video, const std::uint8_t* palette, const std::uint8_t* sprites,
                 const GeometryEngine& geometry, PolygonRenderer& renderer)
    : video_(video),
      palette_(palette),
      sprites_(sprites),
      geometry_(geometry),
      renderer_(renderer),
      main_(DisplayEngine::kMain, video, palette, sprites),
      second_(DisplayEngine::kSub, video, palette + 0x400, sprites + DisplayEngine::kSpriteMemorySize),
      top_(static_cast<std::size_t>(kWidth) * kHeight, 0x3FFFF),
      bottom_(top_) {}

void Display::attach(Arm9Io& io) {
  main_.attach(io);
  second_.attach(io);
  io.add(kCaptureControl, 4, [this] { return capture_control_; }, [this](u32 value, u32 mask) {
    mask &= 0xEF3F1F1F;
    capture_control_ = (capture_control_ & ~mask) | (value & mask);
  });
  io.add(kPowerControl, 2, [this] { return power_control_; }, [this](u32 value, u32 mask) {
    mask &= 0x820F;
    power_control_ = (power_control_ & ~mask) | (value & mask);
  });
}

void Display::line_begins(unsigned line) {
  if (line == 0) {
    {
      frame_log::Scope frame_log_scope(frame_log::kDisplayMain);
      main_.start_frame();
    }
    {
      frame_log::Scope frame_log_scope(frame_log::kDisplaySub);
      second_.start_frame();
    }
    // The polygons handed over at the last vertical blank, if there are new
    // ones, make this frame's 3D picture (or scene).
    const std::uint64_t swaps = geometry_.statistics().swaps;
    const bool capture = (capture_control_ & kCaptureOn) != 0;
    // A capture whose second source is main memory is not made at all
    // (capture_line()); any other is made by the graphics hardware too.
    const bool from_main_memory = ((capture_control_ >> 29) & 3) != 0 && (capture_control_ & kCaptureFromMainMemory) != 0;
    // The captures kept that are still in video memory as the frame begins
    // (this frame's capture can write over one while a screen shows it).
    for (Kept& kept : kept_) kept.in_memory = kept.number != 0 && in_video_memory(kept);
    hardware_capture_ = hardware_3d_ && capture && !from_main_memory;
    if (hardware_capture_) start_hardware_capture();
    if (capture) frame_log::state().counts.capture_made = true;
    scene_this_frame_ = hardware_3d_;
    if (hardware_wanted_ && swaps != 0) {
      if (scene_this_frame_) {
        ++statistics_.frames_3d_hardware;
      } else {
        ++statistics_.frames_3d_processor;
        ++fallbacks_[fallback_.empty() ? std::string("the graphics hardware cannot draw") : fallback_];
      }
    }
    main_.set_hardware_3d(scene_this_frame_);
    // Which engines' pictures the graphics hardware draws. The processor
    // draws the main engine's where a capture takes it, and both while a
    // capture may be on a screen (find_capture() compares them): a recent
    // one, or one shown in the last frame (the game can show a capture for
    // as long as it likes, as behind a message that stops the game).
    bool recent_capture = capture_shown_;
    for (const Kept& kept : kept_)
      if (kept.number != 0 && statistics_.frames + 1 - kept.number <= 3) recent_capture = true;
    const bool by_hardware = hardware_2d_wanted_ && hardware_3d_ && !recent_capture && video_2d_.place(video_);
    by_hardware_[0] = by_hardware && !capture;
    by_hardware_[1] = by_hardware;
    for (Frame2D& f : recording_) {
      f.palettes.clear();
      f.sprites.clear();
    }
    // Video memory as the frame begins: the game changes it at the vertical
    // blank, and can use a bank shown as a place to work while the lines
    // are drawn.
    if (by_hardware) video_2d_.begin(video_, by_hardware_[0], by_hardware_[1]);
    if (scene_this_frame_) {
      if (swaps != swaps_built_) {
        swaps_built_ = swaps;
        frame_log::Scope frame_log_scope(frame_log::kBuildScene);
        if (geometry_.wide() && hud_.on()) {
          hud_.apply_3d(geometry_.wide_finished(), hud_frame_);
          renderer_.build_scene(hud_frame_, video_, true);
        } else if (geometry_.wide()) {
          renderer_.build_scene(geometry_.wide_finished(), video_, true);
        } else {
          renderer_.build_scene(geometry_.finished(), video_);
        }
        have_scene_ = true;
        ++statistics_.pictures_3d;
      }
      // A capture is also written to video memory, where the game can read
      // it back (as a texture or a background, as the start of a battle
      // does): with the 3D picture in it, drawn by the processor.
      if (hardware_capture_ && swaps != swaps_drawn_) {
        swaps_drawn_ = swaps;
        frame_log::Scope frame_log_scope(frame_log::kProcessor3D);
        renderer_.draw(geometry_.finished(), video_);
        have_3d_ = true;
        ++statistics_.pictures_3d_for_captures;
      }
    } else if (swaps != swaps_drawn_) {
      swaps_drawn_ = swaps;
      {
        frame_log::Scope frame_log_scope(frame_log::kProcessor3D);
        renderer_.draw(geometry_.finished(), video_);
      }
      have_3d_ = true;
      ++statistics_.pictures_3d;
      main_.set_3d_picture(&renderer_.image());
    }
  }
  {
    frame_log::Scope frame_log_scope(frame_log::kDisplayMain);
    main_.start_line(static_cast<int>(line));
    if (line == 0) plan_hud();
  }
  frame_log::Scope frame_log_scope(frame_log::kDisplaySub);
  second_.start_line(static_cast<int>(line));
}

void Display::plan_hud() {
  // The widescreen HUD's moves for this frame's 2D picture, from its
  // registers and sprites as it begins, and the elements of the scene it
  // shows. Only with the graphics hardware's wide scene.
  const HudPlan* plan = nullptr;
  if (hud_.on() && scene_this_frame_ && have_scene_ && geometry_.wide()) {
    WideHud::Screen2D screen;
    const u32 control = main_.control();
    screen.sprites = sprites_;
    screen.sprites_on = (control & DisplayEngine::kShowSprites) != 0;
    // The text: background 1, over the 3D layer (background 0).
    const bool text_on = (control & (DisplayEngine::kShowBackground0 << 1)) != 0 &&
                         (control & DisplayEngine::kBackground0Is3D) != 0 && (control & 7) <= 5;
    screen.text_layer = text_on ? 1 : -1;
    screen.has_pixels = [this](unsigned layer, int x0, int y0, int x1, int y1) {
      return main_.has_pixels(layer, x0, y0, x1, y1);
    };
    plan = &hud_.plan_2d(screen);
    if (!plan->on) plan = nullptr;
  } else {
    hud_.clear_plan();
  }
  main_.set_hud(plan);
  if (plan != nullptr) plan->to_words(recording_[0].hud.data());
  else recording_[0].hud.fill(0);
}

void Display::line_done(unsigned line) {
  if (line >= static_cast<unsigned>(kHeight)) return;
  if (by_hardware_[0] || by_hardware_[1]) record_lines(line);
  if (!by_hardware_[0] || check_2d_) {
    frame_log::Scope frame_log_scope(frame_log::kDisplayMain);
    main_.draw_line(static_cast<int>(line));
  }
  if (!by_hardware_[1] || check_2d_) {
    frame_log::Scope frame_log_scope(frame_log::kDisplaySub);
    second_.draw_line(static_cast<int>(line));
  }
  // A capture switched on is made during the next frame, from its first
  // line.
  if (line == 0 && (capture_control_ & kCaptureOn) != 0) capturing_ = true;
  if (capturing_) {
    frame_log::Scope frame_log_scope(frame_log::kCaptureCpu);
    capture_line(line);
  }
}

void Display::record_lines(unsigned line) {
  {
    frame_log::Scope frame_log_scope(frame_log::kDisplayMain);
    video_2d_.line(video_, line);
  }
  for (unsigned e = 0; e < 2; ++e) {
    if (!by_hardware_[e]) continue;
    frame_log::Scope frame_log_scope(e == 0 ? frame_log::kDisplayMain : frame_log::kDisplaySub);
    Frame2D& f = recording_[e];
    // A copy of palette and sprite attribute memory where it changed since
    // the line before.
    const auto copy = [](std::vector<std::uint8_t>& copies, const std::uint8_t* memory) {
      const std::size_t n = copies.size();
      if (n == 0 || std::memcmp(copies.data() + n - Frame2D::kCopySize, memory, Frame2D::kCopySize) != 0)
        copies.insert(copies.end(), memory, memory + Frame2D::kCopySize);
      return static_cast<std::uint32_t>(copies.size() / Frame2D::kCopySize - 1);
    };
    const std::uint32_t palette = copy(f.palettes, palette_ + e * Frame2D::kCopySize);
    const std::uint32_t sprites = copy(f.sprites, sprites_ + e * Frame2D::kCopySize);
    (e == 0 ? main_ : second_).record_line(static_cast<int>(line), f, palette, sprites, !check_2d_);
  }
}

void Display::start_hardware_capture() {
  const unsigned* size = kCaptureSizes[(capture_control_ >> 20) & 3];
  CaptureRecipe& r = recipe_;
  r.source = std::min((capture_control_ >> 29) & 3, 2u);
  r.only_3d = (capture_control_ & kCapture3DOnly) != 0;
  r.weight_first = std::min<u32>(capture_control_ & 0x1F, 16);
  r.weight_second = std::min<u32>((capture_control_ >> 8) & 0x1F, 16);
  r.width = size[0];
  r.height = size[1];
  r.write_start = ((capture_control_ >> 18) & 3) * 0x4000;
  r.read_start = ((capture_control_ >> 26) & 3) * 0x4000;
  r.second_capture = 0;
  r.second_pixels.clear();
  const unsigned into = (capture_control_ >> 16) & 3;
  const unsigned from = (main_.control() >> 18) & 3;
  const Kept* held = nullptr;
  if (r.source != 0) {
    // The second source's bank as it is before this capture writes to it
    // (it can be the same bank): a capture of the graphics hardware if it
    // holds one, wherever the capture reads it; else what is there.
    const u32 base = 0x06800000 + from * 0x20000;
    const std::size_t count = static_cast<std::size_t>(r.width) * r.height;
    for (const Kept& kept : kept_) {
      if (kept.number == 0 || kept.bank != from || (held != nullptr && held->number > kept.number)) continue;
      bool same = true;
      for (std::size_t i = 0; i < count && same; i += 61) {
        const u32 at = (r.read_start + static_cast<u32>(i)) & 0xFFFF;
        same = kept.pixels[at] == six_bits_each(video_.peek(base + at * 2, 2));
      }
      if (same) held = &kept;
    }
    if (held != nullptr) {
      r.second_capture = held->number;
    } else {
      r.second_pixels.resize(0x10000);
      for (u32 at = 0; at < 0x10000; ++at) r.second_pixels[at] = six_bits_each(video_.peek(base + at * 2, 2));
    }
  }
  // In place of a capture no longer in video memory, else of the oldest;
  // not of the one this capture reads. One that a picture shows (a
  // background behind a menu, say) is kept while it is there.
  std::size_t pick = kept_.size();
  for (std::size_t i = 0; i < kept_.size(); ++i) {
    const Kept& k = kept_[i];
    if (&k == held) continue;
    if (pick == kept_.size() || (k.in_memory ? 1 : 0) < (kept_[pick].in_memory ? 1 : 0) ||
        ((k.in_memory == kept_[pick].in_memory) && k.number < kept_[pick].number))
      pick = i;
  }
  kept_at_ = pick;
  Kept& kept = kept_[kept_at_];
  kept.number = 0;
  kept.bank = into;
  kept.pixels.assign(0x10000, kNotCaptured);
  capture_source_.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
}

void Display::capture_line(unsigned line) {
  const unsigned* size = kCaptureSizes[(capture_control_ >> 20) & 3];
  const unsigned width = size[0], height = size[1];
  // Into one of banks A to D, placed as plain memory, from one of four
  // offsets; places wrap inside the bank.
  const u32 into = 0x06800000 + ((capture_control_ >> 16) & 3) * 0x20000;
  const u32 write_at = ((capture_control_ >> 18) & 3) * 0x8000 + line * width * 2;
  const u32 from = 0x06800000 + ((main_.control() >> 18) & 3) * 0x20000;
  const u32 read_at = ((capture_control_ >> 26) & 3) * 0x8000 + line * width * 2;
  const unsigned source = (capture_control_ >> 29) & 3;

  // The first source: the main engine's picture before the display mode and
  // the master brightness, or the 3D picture alone.
  // With the graphics hardware's scene, the processor's 3D picture is put
  // in as the scene would be.
  const bool only_3d = (capture_control_ & kCapture3DOnly) != 0;
  const bool picture_3d = have_3d_ && line < static_cast<unsigned>(kHeight);
  const auto* marks = hardware_capture_ && !only_3d ? main_.line_marks() : nullptr;
  const auto first = [&](unsigned x) {
    const u32 pixel_3d = picture_3d ? renderer_.image()[static_cast<std::size_t>(line) * kWidth + x] : 0;
    if (!only_3d) {
      const u32 below = main_.mixed_line()[x];
      return five_bits_each(marks != nullptr && picture_3d ? put_in_3d(below, (*marks)[x], pixel_3d) : below);
    }
    return picture_3d ? five_bits_each(pixel_3d) : u32{0x8000};
  };
  // The second: a bank of video memory, as 16 bits a pixel.
  const auto second = [&](unsigned x) { return video_.peek(from + ((read_at + x * 2) & 0x1FFFF), 2); };
  if (source != 0 && (capture_control_ & kCaptureFromMainMemory) != 0) {
    ++notes_["a capture from the display fed from main memory"];
  } else {
    const u32 weight_first = std::min<u32>(capture_control_ & 0x1F, 16);
    const u32 weight_second = std::min<u32>((capture_control_ >> 8) & 0x1F, 16);
    // Where the graphics hardware puts the 3D scene, two captures of the
    // same picture are the same. The lowest bits there tell this capture
    // from the ones made before it, to find which one an engine shows.
    const auto is_3d = [&](unsigned x) {
      if (!hardware_capture_ || source == 1) return false;
      return only_3d || (marks != nullptr && ((*marks)[x] & DisplayEngine::kMark3D) != 0);
    };
    const u32 sign = (statistics_.frames + 1) & 7;
    const u32 signature = (sign & 1) | ((sign >> 1) & 1) << 5 | ((sign >> 2) & 1) << 10;
    for (unsigned x = 0; x < width; ++x) {
      u32 pixel;
      if (source == 0) {
        pixel = first(x);
      } else if (source == 1) {
        pixel = second(x);
      } else {
        // Sixteenths of each, five bits a part.
        const u32 a = first(x), b = second(x);
        pixel = 0x8000;
        for (unsigned shift = 0; shift < 15; shift += 5)
          pixel |= std::min<u32>((((a >> shift) & 0x1F) * weight_first + ((b >> shift) & 0x1F) * weight_second) / 16, 31) << shift;
      }
      if (is_3d(x)) pixel ^= signature;
      const u32 place = (write_at + x * 2) & 0x1FFFF;
      video_.poke(into + place, 2, pixel);
      if (hardware_capture_) {
        // What the graphics hardware's capture takes, and what this one put
        // in video memory, to find where it is shown.
        if (x < static_cast<unsigned>(kWidth) && line < static_cast<unsigned>(kHeight))
          capture_source_[static_cast<std::size_t>(line) * kWidth + x] =
              main_.mixed_line()[x] | (marks != nullptr ? (*marks)[x] : 0);
        kept_[kept_at_].pixels[place / 2] = six_bits_each(pixel);
      }
    }
  }
  if (line + 1 == height) {
    capturing_ = false;
    capture_control_ &= ~kCaptureOn;
    ++statistics_.captures;
    if (hardware_capture_) kept_[kept_at_].number = statistics_.frames + 1;
  }
}

void Display::vertical_blank() {
  frame_log::Scope frame_log_scope(frame_log::kCaptureCpu);
  // The frame is complete: each engine's picture goes to its screen.
  const bool main_on_top = (power_control_ & kMainOnTop) != 0;
  main_on_top_ = main_on_top;
  // The pictures the processor drew; the lines the graphics hardware is to
  // draw.
  if (!by_hardware_[0] || check_2d_) (main_on_top ? top_ : bottom_) = main_.screen();
  if (!by_hardware_[1] || check_2d_) (main_on_top ? bottom_ : top_) = second_.screen();
  for (unsigned e = 0; e < 2; ++e) {
    recording_[e].by_hardware = by_hardware_[e];
    recording_[e].second_engine = e == 1;
    std::swap(frames_2d_[e], recording_[e]);
  }
  if (by_hardware_[0] || by_hardware_[1]) {
    if (const std::uint64_t left_out = video_2d_.finish(); left_out != 0)
      notes_["video memory changed while the lines were drawn, beyond what the graphics hardware is given (blocks)"] +=
          left_out;
  }
  frame_capture_ = hardware_capture_ ? kept_[kept_at_].number : 0;
  if (frame_capture_ != 0) {
    // A kept capture in the picture captured (as a background turned and
    // made smaller, as the start of a battle does), put in by the graphics
    // hardware at its resolution.
    recipe_.put_capture = 0;
    Shown shown;
    if (!recipe_.only_3d && recipe_.source != 1) {
      shown = shown_capture(main_, capture_source_, true);
      if (shown.kept != nullptr) {
        put_in(main_, shown, capture_source_, capture_source_, recipe_.put_lines);
        recipe_.put_capture = shown.kept->number;
      }
    }
    capture_sides_.clear();
    if (main_.sides() && !main_.side_mixed().empty()) make_sides(main_.side_mixed(), shown, capture_sides_);
    std::swap(frame_recipe_, recipe_);
  }
  ScreenFill& main_fill = frame_fills_[main_on_top ? 0 : 1];
  ScreenFill& second_fill = frame_fills_[main_on_top ? 1 : 0];
  main_fill = second_fill = ScreenFill{};
  if (scene_this_frame_ && have_scene_ && ((main_.control() >> 16) & 3) == 1)
    main_fill = {Fill::kScene, 0, main_.master_brightness(), {}};
  // A capture can be on a screen with the scene too, as behind a message
  // box the game draws in 3D.
  Shown main_shown;
  if (!by_hardware_[0]) main_shown = find_capture(main_, main_on_top ? top_ : bottom_, main_fill);
  if (!by_hardware_[1]) find_capture(second_, main_on_top ? bottom_ : top_, second_fill);
  // A capture the game shows (not one put in at the sides only, below).
  capture_shown_ = main_fill.fill == Fill::kCapture || second_fill.fill == Fill::kCapture || main_fill.capture != 0 ||
                   second_fill.capture != 0;
  // The main engine's sides, by its display mode: the picture's; the bank
  // shown, which only a kept capture has; white; or black.
  for (auto& sides : screen_sides_) sides.clear();
  if (main_.sides() && !by_hardware_[0] && !main_.side_shown().empty()) {
    std::vector<std::uint32_t>& sides = screen_sides_[main_on_top ? 0 : 1];
    switch ((main_.control() >> DisplayEngine::kDisplayModeShift) & 3) {
      case 1: make_sides(main_.side_shown(), main_shown, sides); break;
      case 2:
        sides.assign(main_.side_shown().size(), main_shown.kept != nullptr && main_shown.background < 0
                                                     ? DisplayEngine::kCapturedBelow
                                                     : 0u);
        break;
      case 0: sides.assign(main_.side_shown().size(), 0x3FFFFu); break;
      default: sides.assign(main_.side_shown().size(), 0u); break;
    }
    if (main_shown.kept == nullptr && ((main_.control() >> DisplayEngine::kDisplayModeShift) & 3) == 1)
      capture_at_sides(main_fill, &sides);
  }
  if (by_hardware_[0] && main_.sides()) capture_at_sides(main_fill, nullptr);
  ++statistics_.frames;
}

bool Display::in_video_memory(const Kept& kept) const {
  // One pixel in 61 of those it wrote, in the bank wherever it is placed
  // now (to be shown, the game places it as a background, say).
  const std::vector<std::uint8_t>& bank = video_.contents(static_cast<VideoMemory::Bank>(kept.bank));
  for (u32 at = 0; at < 0x10000; at += 61) {
    const u32 pixel = kept.pixels[at];
    if (pixel != kNotCaptured && six_bits_each(bank[at * 2] | u32{bank[at * 2 + 1]} << 8) != pixel) return false;
  }
  return true;
}

int Display::bitmap_row(const DisplayEngine& engine, int background, int line, unsigned bank) const {
  return bitmap_row(engine.bitmap_line(static_cast<unsigned>(background), line), bank);
}

int Display::bitmap_row(const DisplayEngine::BitmapLine& b, unsigned bank) const {
  if (!b.on || b.width != 256) return -1;
  VideoMemory::Bank where;
  u32 offset = 0;
  if (!video_.locate(b.data, where, offset) || static_cast<unsigned>(where) != bank || (offset & 0x1FF) != 0) return -1;
  return static_cast<int>(offset / 512);
}

namespace {

constexpr u32 kColour = 0x3FFFF;

// The pixel of the bank that bitmap line `b`, from row `row`, shows at `x`;
// -1 for none. As DisplayEngine::draw_extended() finds it.
int bank_pixel(const DisplayEngine::BitmapLine& b, int row, int x) {
  int bx = (b.x0 + b.pa * x) >> 8, by = (b.y0 + b.pc * x) >> 8;
  if (b.repeated) {
    bx &= 255;
    by &= b.height - 1;
  } else if (bx < 0 || bx >= 256 || by < 0 || by >= b.height) {
    return -1;
  }
  return ((row + by) & 255) * 256 + bx;
}

}  // namespace

Display::Shown Display::shown_capture(const DisplayEngine& engine, const std::vector<std::uint32_t>& colours,
                                      bool through_backgrounds) const {
  Shown best;
  unsigned best_count = 0;
  for (const Kept& kept : kept_) {
    // A capture made in this frame is not complete until the frame is: what
    // the engines show now can only be an earlier one. Only one still in
    // video memory as it was made, as the frame began, can be shown.
    if (kept.number == 0 || kept.number > statistics_.frames || !kept.in_memory) continue;
    for (int background = -1; background <= 3; ++background) {
      if (background == 0 || background == 1 || (background >= 2 && !through_backgrounds)) continue;
      unsigned count = 0, samples = 0;
      if (background < 0) {
        // Where the screen would show its bank (as display mode 2 does).
        for (std::size_t i = 0; i < colours.size(); i += 61) {
          if (kept.pixels[i] == kNotCaptured) continue;
          ++samples;
          count += (colours[i] & kColour) == kept.pixels[i] ? 1 : 0;
        }
      } else {
        for (int line = 0; line < kHeight; line += 3) {
          const int row = bitmap_row(engine, background, line, kept.bank);
          if (row < 0) continue;
          const DisplayEngine::BitmapLine& b = engine.bitmap_line(static_cast<unsigned>(background), line);
          for (int x = line % 7; x < kWidth; x += 7) {
            const int at = bank_pixel(b, row, x);
            if (at < 0 || kept.pixels[static_cast<std::size_t>(at)] == kNotCaptured) continue;
            ++samples;
            count += (colours[static_cast<std::size_t>(line) * kWidth + x] & kColour) ==
                             kept.pixels[static_cast<std::size_t>(at)]
                         ? 1
                         : 0;
          }
        }
        if (samples < 16) continue;
      }
      if (count * 4 < samples * 3 || samples == 0) continue;
      if (count > best_count || (best.kept != nullptr && count == best_count && kept.number > best.kept->number)) {
        best_count = count;
        best = {&kept, background};
      }
    }
  }
  return best;
}

void Display::put_in(const DisplayEngine& engine, const Shown& shown, const std::vector<std::uint32_t>& colours,
                     std::vector<std::uint32_t>& marked, CaptureLines& lines) const {
  const Kept& kept = *shown.kept;
  for (int line = 0; line < kHeight; ++line) {
    CaptureLines::Line& l = lines.lines[static_cast<std::size_t>(line)];
    l = CaptureLines::Line{};
    const std::size_t first = static_cast<std::size_t>(line) * kWidth;
    if (shown.background < 0) {
      l.on = true;
      l.y0 = line * 256;
      for (std::size_t i = first; i < first + kWidth; ++i)
        if (kept.pixels[i] != kNotCaptured && (colours[i] & kColour) == kept.pixels[i])
          marked[i] |= DisplayEngine::kCapturedBelow;
      continue;
    }
    const int row = bitmap_row(engine, shown.background, line, kept.bank);
    if (row < 0) continue;
    const DisplayEngine::BitmapLine& b = engine.bitmap_line(static_cast<unsigned>(shown.background), line);
    l = {true, b.repeated, b.x0, b.y0, b.pa, b.pb, b.pc, b.pd, static_cast<std::uint16_t>(row), b.height};
    for (int x = 0; x < kWidth; ++x) {
      const int at = bank_pixel(b, row, x);
      if (at < 0 || kept.pixels[static_cast<std::size_t>(at)] == kNotCaptured) continue;
      if ((colours[first + static_cast<std::size_t>(x)] & kColour) == kept.pixels[static_cast<std::size_t>(at)])
        marked[first + static_cast<std::size_t>(x)] |= DisplayEngine::kCapturedBelow;
    }
  }
}

void Display::make_sides(const std::vector<std::uint32_t>& from, const Shown& shown,
                         std::vector<std::uint32_t>& sides) const {
  sides = from;
  if (shown.kept == nullptr || shown.background < 0) return;
  // Where the background that shows it is on top; and where nothing is,
  // if it is a background that goes on at the sides (a bitmap that does
  // not repeat ends at the screen's edge, but the capture is wider).
  u32 words[Frame2D::kWords] = {};
  words[0] = main_.control();
  words[1] = u32{main_.background_control(0)} | u32{main_.background_control(1)} << 16;
  words[2] = u32{main_.background_control(2)} | u32{main_.background_control(3)} << 16;
  const bool under = shown.background >= 0 && (DisplayEngine::side_backgrounds(words) & (1u << shown.background)) != 0;
  const std::vector<std::uint8_t>& layers = main_.side_layers();
  for (std::size_t i = 0; i < sides.size() && i < layers.size(); ++i)
    if (layers[i] == shown.background || (under && layers[i] == 5)) sides[i] |= DisplayEngine::kCapturedBelow;
}

Display::Blur Display::blur_between(const std::vector<std::uint32_t>& kept, const std::vector<std::uint8_t>& bank) {
  Blur found;
  if (kept.size() != 0x10000 || bank.size() < 0x20000) return found;
  // At one pixel in 61 of those the capture wrote: the pixel now, and the
  // capture's blurred by each box, a part each.
  std::vector<std::array<int, 3 * (kMostBlur + 2)>> samples;
  for (u32 at = 0; at < 0x10000; at += 61) {
    if (kept[at] == kNotCaptured) continue;
    std::array<int, 3 * (kMostBlur + 2)> sample{};
    const u32 now = six_bits_each(bank[at * 2] | u32{bank[at * 2 + 1]} << 8);
    for (int k = 0; k < 3; ++k) sample[static_cast<std::size_t>(k)] = static_cast<int>((now >> (6 * k)) & 63);
    const int x = static_cast<int>(at & 255), y = static_cast<int>(at >> 8);
    for (int r = 0; r <= kMostBlur; ++r) {
      u32 sum[3] = {}, n = 0;
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          const u32 p = kept[static_cast<std::size_t>(std::clamp(y + dy, 0, 255) * 256 + std::clamp(x + dx, 0, 255))];
          if (p == kNotCaptured) continue;
          for (int k = 0; k < 3; ++k) sum[k] += (p >> (6 * k)) & 63;
          ++n;
        }
      }
      for (int k = 0; k < 3; ++k)
        sample[static_cast<std::size_t>(3 * (r + 1) + k)] = static_cast<int>((sum[k] + n / 2) / n);
    }
    samples.push_back(sample);
  }
  if (samples.size() < 64) return found;
  const auto count = static_cast<std::int64_t>(samples.size());
  std::int64_t best_error = -1;
  for (int r = 0; r <= kMostBlur; ++r) {
    // The mean difference of each part, then the mean distance beyond it.
    std::array<std::int64_t, 3> difference{};
    for (const auto& sample : samples)
      for (std::size_t k = 0; k < 3; ++k) difference[k] += sample[k] - sample[3 * static_cast<std::size_t>(r + 1) + k];
    std::array<std::int8_t, 3> offset{};
    for (std::size_t k = 0; k < 3; ++k)
      offset[k] = static_cast<std::int8_t>(std::clamp<std::int64_t>((difference[k] + count / 2) / count, -16, 16));
    std::int64_t error = 0;
    for (const auto& sample : samples)
      for (std::size_t k = 0; k < 3; ++k)
        error += std::abs(sample[k] - std::clamp(sample[3 * static_cast<std::size_t>(r + 1) + k] + offset[k], 0, 63));
    if (best_error < 0 || error < best_error) {
      best_error = error;
      found = {r, offset};
    }
  }
  // Near: within two and a half sixty-fourths a part, on average (video
  // memory keeps five bits a part, and the game's sums round).
  if (best_error * 2 > 5 * 3 * count) found = Blur{};
  return found;
}

void Display::capture_at_sides(ScreenFill& fill, std::vector<std::uint32_t>* sides) {
  // Each line's registers: as recorded for the graphics hardware, or, where
  // the processor drew the line, the background controls at the end.
  const bool hardware = sides == nullptr;
  Frame2D& frame = frames_2d_[0];
  u32 now[Frame2D::kWords] = {};
  now[0] = main_.control();
  now[1] = u32{main_.background_control(0)} | u32{main_.background_control(1)} << 16;
  now[2] = u32{main_.background_control(2)} | u32{main_.background_control(3)} << 16;
  const auto words = [&](int line) {
    return hardware ? &frame.lines[static_cast<std::size_t>(line) * Frame2D::kWords] : now;
  };
  // Of the backgrounds that can go on at the sides on `line` (background 2
  // before 3: nearer the top, if as near), the first that is a bitmap in
  // `bank`, and its row there; -1 for none.
  const auto row_of = [&](int line, unsigned bank, int& background, DisplayEngine::BitmapLine& b) {
    const unsigned candidates = DisplayEngine::side_backgrounds(words(line));
    const unsigned order[2] = {
        (words(line)[2] & 3) <= ((words(line)[2] >> 16) & 3) ? 2u : 3u,
        (words(line)[2] & 3) <= ((words(line)[2] >> 16) & 3) ? 3u : 2u};
    for (const unsigned n : order) {
      if ((candidates & (1u << n)) == 0) continue;
      b = hardware ? DisplayEngine::bitmap_line_of(words(line), n - 2) : main_.bitmap_line(n, line);
      const int row = b.on ? bitmap_row(b, bank) : -1;
      if (row < 0) continue;
      background = static_cast<int>(n);
      return row;
    }
    return -1;
  };
  // The kept capture in the bank most lines show there.
  const Kept* chosen = nullptr;
  unsigned chosen_lines = 0;
  for (const Kept& kept : kept_) {
    if (kept.number == 0 || kept.number > statistics_.frames) continue;
    unsigned count = 0;
    for (int line = 0; line < kHeight; ++line) {
      int background;
      DisplayEngine::BitmapLine b;
      if (row_of(line, kept.bank, background, b) >= 0) ++count;
    }
    if (count > chosen_lines || (count != 0 && count == chosen_lines && kept.number > chosen->number)) {
      chosen = &kept;
      chosen_lines = count;
    }
  }
  if (chosen == nullptr) return;
  const Blur blur = blur_between(chosen->pixels, video_.contents(static_cast<VideoMemory::Bank>(chosen->bank)));
  if (blur.radius < 0) return;
  const std::vector<std::uint8_t>& layers = main_.side_layers();
  constexpr std::size_t kAcross = 2 * DisplayEngine::kSideColumns;
  for (int line = 0; line < kHeight; ++line) {
    int background;
    DisplayEngine::BitmapLine b;
    const int row = row_of(line, chosen->bank, background, b);
    if (row < 0) continue;
    if (hardware) {
      words(line)[23] |= 1u << (background - 2);
    } else {
      // Where that background is on top, or nothing is.
      for (std::size_t i = static_cast<std::size_t>(line) * kAcross; i < (static_cast<std::size_t>(line) + 1) * kAcross &&
                                                                     i < sides->size() && i < layers.size();
           ++i)
        if (layers[i] == background || layers[i] == 5) (*sides)[i] |= DisplayEngine::kCapturedBelow;
    }
    fill.lines.lines[static_cast<std::size_t>(line)] = {true, b.repeated, b.x0, b.y0, b.pa, b.pb, b.pc, b.pd,
                                                        static_cast<std::uint16_t>(row), b.height};
  }
  fill.lines.sides_only = true;
  fill.lines.blur = static_cast<std::uint8_t>(blur.radius);
  fill.lines.offset = blur.offset;
  fill.capture = chosen->number;
  if (fill.fill == Fill::kNone) {
    fill.fill = Fill::kCapture;
    fill.brightness = main_.master_brightness();
  }
}

Display::Shown Display::find_capture(const DisplayEngine& engine, std::vector<std::uint32_t>& screen,
                                     ScreenFill& fill) const {
  // Through a background only where the screen shows the engine's picture.
  const bool picture = ((engine.control() >> DisplayEngine::kDisplayModeShift) & 3) == 1;
  const Shown shown = shown_capture(engine, engine.unlit_screen(), picture);
  if (shown.kept == nullptr) return shown;
  put_in(engine, shown, engine.unlit_screen(), screen, fill.lines);
  if (fill.fill != Fill::kScene) {
    fill.fill = Fill::kCapture;
    fill.brightness = engine.master_brightness();
  }
  fill.capture = shown.kept->number;
  return shown;
}

}  // namespace bd::nds
