#include "nds/display.h"

#include <algorithm>

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
    const bool main_on_top = (power_control_ & kMainOnTop) != 0;
    // 3D on both screens: all of the main engine's picture (source A, 256
    // by 192) captured in this frame and the last, the main engine on the
    // other screen now.
    hardware_capture_ = hardware_3d_ && capture && (capture_control_ & (3u << 29 | kCapture3DOnly)) == 0 &&
                        ((capture_control_ >> 20) & 3) == 3 && last_capture_frame_ == statistics_.frames &&
                        main_on_top != last_main_on_top_;
    if (capture) {
      last_capture_frame_ = statistics_.frames + 1;
      last_main_on_top_ = main_on_top;
    }
    if (hardware_capture_) {
      kept_at_ = (kept_at_ + 1) % kept_.size();
      kept_[kept_at_].number = 0;
      kept_[kept_at_].pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
      capture_source_.resize(static_cast<std::size_t>(kWidth) * kHeight);
    }
    scene_this_frame_ = hardware_3d_ && (!capture || hardware_capture_);
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
  const auto first = [&](unsigned x) {
    if ((capture_control_ & kCapture3DOnly) == 0) return five_bits_each(main_.mixed_line()[x]);
    if (!have_3d_) return u32{0x8000};
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
    const auto* marks = hardware_capture_ ? main_.line_marks() : nullptr;
    const u32 sign = (statistics_.frames + 1) & 7;
    const u32 signature = (sign & 1) | ((sign >> 1) & 1) << 5 | ((sign >> 2) & 1) << 10;
    for (unsigned x = 0; x < width; ++x) {
      u32 pixel;
      if (source == 0) {
        pixel = first(x);
        if (marks != nullptr && ((*marks)[x] & DisplayEngine::kMark3D) != 0) pixel ^= signature;
      } else if (source == 1) {
        pixel = second(x);
      } else {
        // Sixteenths of each, five bits a part.
        const u32 a = first(x), b = second(x);
        pixel = 0x8000;
        for (unsigned shift = 0; shift < 15; shift += 5)
          pixel |= std::min<u32>((((a >> shift) & 0x1F) * weight_first + ((b >> shift) & 0x1F) * weight_second) / 16, 31) << shift;
      }
      video_.poke(into + ((write_at + x * 2) & 0x1FFFF), 2, pixel);
      if (hardware_capture_) {
        // What the graphics hardware's capture takes, and what this one put
        // in video memory, to find where it is shown.
        const std::size_t at = static_cast<std::size_t>(line) * kWidth + x;
        capture_source_[at] = main_.mixed_line()[x] | (marks != nullptr ? (*marks)[x] : 0);
        kept_[kept_at_].pixels[at] = six_bits_each(pixel);
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
  // The recent capture the engine shows most of, at one pixel in 61; it
  // counts if three quarters of those are the same.
  const std::vector<std::uint32_t>& shown = engine.unlit_screen();
  const Kept* best = nullptr;
  unsigned best_count = 0, samples = 0;
  for (const Kept& kept : kept_) {
    // A capture made in this frame is not complete until the frame is: what
    // the engines show now can only be an earlier one.
    if (kept.number == 0 || kept.number > statistics_.frames || statistics_.frames + 1 - kept.number > 3) continue;
    unsigned count = 0;
    samples = 0;
    for (std::size_t i = 0; i < shown.size(); i += 61, ++samples) count += shown[i] == kept.pixels[i] ? 1 : 0;
    if (count > best_count || (best != nullptr && count == best_count && kept.number > best->number)) {
      best_count = count;
      best = &kept;
    }
  }
  if (best == nullptr || best_count * 4 < samples * 3) return;
  for (std::size_t i = 0; i < shown.size(); ++i)
    if (shown[i] == best->pixels[i]) screen[i] |= DisplayEngine::kMark3D;
  fill = {Fill::kCapture, best->number, engine.master_brightness()};
}

}  // namespace bd::nds
