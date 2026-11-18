#include "nds/display.h"

#include <algorithm>

namespace bd::nds {

namespace {

using u32 = std::uint32_t;

// Six bits a part made five, with the bit that says "a pixel is here".
u32 five_bits_each(u32 colour) {
  return 0x8000 | ((colour >> 1) & 0x1F) | (((colour >> 7) & 0x1F) << 5) | (((colour >> 13) & 0x1F) << 10);
}

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
    scene_this_frame_ = hardware_3d_ && (capture_control_ & kCaptureOn) == 0;
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
      video_.poke(into + ((write_at + x * 2) & 0x1FFFF), 2, pixel);
    }
  }
  if (line + 1 == height) {
    capturing_ = false;
    capture_control_ &= ~kCaptureOn;
    ++statistics_.captures;
  }
}

void Display::vertical_blank() {
  // The frame is complete: each engine's picture goes to its screen.
  const bool main_on_top = (power_control_ & kMainOnTop) != 0;
  top_ = main_on_top ? main_.screen() : second_.screen();
  bottom_ = main_on_top ? second_.screen() : main_.screen();
  frame_has_scene_ = scene_this_frame_ && have_scene_;
  frame_brightness_ = main_.master_brightness();
  frame_main_on_top_ = main_on_top;
  ++statistics_.frames;
}

}  // namespace bd::nds
