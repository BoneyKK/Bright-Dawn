#include "nds/display.h"

#include <algorithm>
#include <utility>

namespace bd::nds {

namespace {

using u32 = std::uint32_t;

// Six bits a part made five, with the bit that says "a pixel is here".
u32 five_bits_each(u32 colour) {
  return 0x8000 | ((colour >> 1) & 0x1F) | (((colour >> 7) & 0x1F) << 5) | (((colour >> 13) & 0x1F) << 10);
}

// Five bits a part made six, as the display engines read video memory.
u32 six_bits_each(u32 colour) { return ((colour & 0x1F) << 1) | ((colour & 0x3E0) << 2) | ((colour & 0x7C00) << 3); }

// The sizes a capture can have, across and down.
constexpr unsigned kCaptureSizes[4][2] = {{128, 128}, {256, 64}, {256, 128}, {256, 192}};

}  // namespace

Display::Display(VideoMemory& video, const std::uint8_t* palette, const std::uint8_t* sprites,
                 const GeometryEngine& geometry, PolygonRenderer& renderer)
    : video_(video),
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
    main_.start_frame();
    second_.start_frame();
    // The polygons handed over at the last vertical blank, if there are new
    // ones, make this frame's 3D picture (or scene).
    const std::uint64_t swaps = geometry_.statistics().swaps;
    const bool capture = (capture_control_ & kCaptureOn) != 0;
    // A capture whose second source is main memory is not made at all
    // (capture_line()); any other is made by the graphics hardware too.
    const bool from_main_memory = ((capture_control_ >> 29) & 3) != 0 && (capture_control_ & kCaptureFromMainMemory) != 0;
    hardware_capture_ = hardware_3d_ && capture && !from_main_memory;
    if (hardware_capture_) start_hardware_capture();
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
    if (scene_this_frame_) {
      if (swaps != swaps_built_) {
        swaps_built_ = swaps;
        renderer_.build_scene(geometry_.finished(), video_);
        have_scene_ = true;
        ++statistics_.pictures_3d;
      }
    } else if (swaps != swaps_drawn_) {
      swaps_drawn_ = swaps;
      renderer_.draw(geometry_.finished(), video_);
      have_3d_ = true;
      ++statistics_.pictures_3d;
      main_.set_3d_picture(&renderer_.image());
    }
  }
  main_.start_line(static_cast<int>(line));
  second_.start_line(static_cast<int>(line));
}

void Display::line_done(unsigned line) {
  if (line >= static_cast<unsigned>(kHeight)) return;
  main_.draw_line(static_cast<int>(line));
  second_.draw_line(static_cast<int>(line));
  // A capture switched on is made during the next frame, from its first
  // line.
  if (line == 0 && (capture_control_ & kCaptureOn) != 0) capturing_ = true;
  if (capturing_) capture_line(line);
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
  if (r.source != 0) {
    // The second source's bank as it is before this capture writes to it
    // (it can be the same bank): a capture of the graphics hardware if it
    // holds one, wherever the capture reads it; else what is there.
    const u32 base = 0x06800000 + from * 0x20000;
    const std::size_t count = static_cast<std::size_t>(r.width) * r.height;
    const Kept* held = nullptr;
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
  kept_at_ = (kept_at_ + 1) % kept_.size();
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
  const bool only_3d = (capture_control_ & kCapture3DOnly) != 0;
  const auto first = [&](unsigned x) {
    if (!only_3d) return five_bits_each(main_.mixed_line()[x]);
    if (!have_3d_ || hardware_capture_) return u32{0x8000};
    return five_bits_each(renderer_.image()[static_cast<std::size_t>(line) * kWidth + x]);
  };
  // The second: a bank of video memory, as 16 bits a pixel.
  const auto second = [&](unsigned x) { return video_.peek(from + ((read_at + x * 2) & 0x1FFFF), 2); };
  if (source != 0 && (capture_control_ & kCaptureFromMainMemory) != 0) {
    ++notes_["a capture from the display fed from main memory"];
  } else {
    const u32 weight_first = std::min<u32>(capture_control_ & 0x1F, 16);
    const u32 weight_second = std::min<u32>((capture_control_ >> 8) & 0x1F, 16);
    // Where the graphics hardware puts the 3D scene, the processor has no
    // colour for it, and a picture all 3D is the same in every capture. The
    // lowest bits there tell this capture from the ones made before it, to
    // find which one an engine shows.
    const auto* marks = hardware_capture_ && !only_3d ? main_.line_marks() : nullptr;
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
  // The frame is complete: each engine's picture goes to its screen.
  const bool main_on_top = (power_control_ & kMainOnTop) != 0;
  top_ = main_on_top ? main_.screen() : second_.screen();
  bottom_ = main_on_top ? second_.screen() : main_.screen();
  frame_capture_ = hardware_capture_ ? kept_[kept_at_].number : 0;
  if (frame_capture_ != 0) std::swap(frame_recipe_, recipe_);
  ScreenFill& main_fill = frame_fills_[main_on_top ? 0 : 1];
  ScreenFill& second_fill = frame_fills_[main_on_top ? 1 : 0];
  main_fill = second_fill = ScreenFill{};
  if (scene_this_frame_ && have_scene_ && ((main_.control() >> 16) & 3) == 1)
    main_fill = {Fill::kScene, 0, main_.master_brightness()};
  else
    find_capture(main_, main_on_top ? top_ : bottom_, main_fill);
  find_capture(second_, main_on_top ? bottom_ : top_, second_fill);
  ++statistics_.frames;
}

void Display::find_capture(const DisplayEngine& engine, std::vector<std::uint32_t>& screen, ScreenFill& fill) const {
  // The recent capture the engine shows most of, at one pixel in 61, where
  // the screen would show its bank (as display mode 2 does); it counts if
  // three quarters of those it covers are the same.
  const std::vector<std::uint32_t>& shown = engine.unlit_screen();
  const Kept* best = nullptr;
  unsigned best_count = 0, best_samples = 0;
  for (const Kept& kept : kept_) {
    // A capture made in this frame is not complete until the frame is: what
    // the engines show now can only be an earlier one.
    if (kept.number == 0 || kept.number > statistics_.frames || statistics_.frames + 1 - kept.number > 3) continue;
    unsigned count = 0, samples = 0;
    for (std::size_t i = 0; i < shown.size(); i += 61) {
      if (kept.pixels[i] == kNotCaptured) continue;
      ++samples;
      count += shown[i] == kept.pixels[i] ? 1 : 0;
    }
    if (count * 4 < samples * 3 || samples == 0) continue;
    if (count > best_count || (best != nullptr && count == best_count && kept.number > best->number)) {
      best_count = count;
      best_samples = samples;
      best = &kept;
    }
  }
  if (best == nullptr || best_samples == 0) return;
  for (std::size_t i = 0; i < shown.size(); ++i)
    if (shown[i] == best->pixels[i]) screen[i] |= DisplayEngine::kMark3D;
  fill = {Fill::kCapture, best->number, engine.master_brightness()};
}

}  // namespace bd::nds
