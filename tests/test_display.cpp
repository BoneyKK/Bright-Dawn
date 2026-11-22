// The display as a whole: the two engines on their screens, the 3D picture,
// and the display capture.
#include <array>
#include <cstdint>

#include "nds/arm9_io.h"
#include "nds/display.h"
#include "nds/geometry_engine.h"
#include "nds/interrupts.h"
#include "nds/polygon_renderer.h"
#include "nds/video_memory.h"
#include "test.h"

namespace {

using bd::nds::Display;
using bd::nds::DisplayEngine;
using bd::nds::GeometryEngine;
using bd::nds::PolygonRenderer;
using bd::nds::VideoMemory;
using u32 = std::uint32_t;

constexpr u32 rgb(u32 r, u32 g, u32 b) { return r | g << 6 | b << 12; }
constexpr u32 kRed15 = 0x001F, kGreen15 = 0x03E0;
constexpr u32 kRed = rgb(62, 0, 0), kGreen = rgb(0, 62, 0);
constexpr u32 kPicture = 1u << 16;

struct Fixture {
  bd::nds::Arm9Io io;
  bd::nds::Interrupts irq;
  VideoMemory video;
  std::array<std::uint8_t, 0x800> palette{}, sprites{};
  GeometryEngine geometry{irq};
  PolygonRenderer renderer;
  Display display{video, palette.data(), sprites.data(), geometry, renderer};

  Fixture() {
    display.attach(io);
    renderer.attach(io);
    geometry.attach(io);
    io.write(PolygonRenderer::kClearDepth, 2, 0x7FFF);
    // The backdrops: red for the main engine, green for the second.
    backdrop(false, kRed15);
    backdrop(true, kGreen15);
    // Banks A and B as plain memory, for captures.
    video.set_control(VideoMemory::kA, VideoMemory::kEnable);
    video.set_control(VideoMemory::kB, VideoMemory::kEnable);
  }
  void backdrop(bool second, u32 colour) {
    palette[second ? 0x400 : 0] = static_cast<std::uint8_t>(colour);
    palette[(second ? 0x400 : 0) + 1] = static_cast<std::uint8_t>(colour >> 8);
  }
  // A whole frame, as the display's timing would drive it.
  void frame() {
    for (unsigned line = 0; line < 263; ++line) {
      display.line_begins(line);
      if (line < 192) display.line_done(line);
      if (line == 192) {
        display.vertical_blank();
        geometry.vertical_blank();
      }
    }
  }
  // A pixel of bank A or B as 16 bits.
  u32 bank_a(u32 x, u32 line, u32 across = 256) { return video.read(0x06800000 + (line * across + x) * 2, 2); }
  u32 bank_b(u32 x, u32 line, u32 across = 256) { return video.read(0x06820000 + (line * across + x) * 2, 2); }
  // A red triangle over the lower left half of the screen, and "swap
  // buffers".
  void send_triangle() {
    io.write(0x040004A4, 4, 0x001F00C0);
    io.write(0x04000500, 4, 0);
    io.write(0x04000480, 4, 0x001F);
    io.write(0x04000490, 4, 0x3C0u | 0x040u << 10);
    io.write(0x04000490, 4, 0x3C0u | 0x3C0u << 10);
    io.write(0x04000490, 4, 0x040u | 0x3C0u << 10);
    io.write(0x04000540, 4, 0);
  }
};

}  // namespace

TEST(display_registers_of_the_whole) {
  Fixture f;
  f.io.write(Display::kCaptureControl, 4, 0xFFFFFFFF);
  f.io.write(Display::kPowerControl, 2, 0xFFFF);
  CHECK(f.io.read(Display::kCaptureControl, 4) == 0xEF3F1F1F && f.io.read(Display::kPowerControl, 2) == 0x820F);
  // Both engines' registers are there.
  f.io.write(0x04000000, 4, kPicture);
  f.io.write(0x04001000, 4, kPicture);
  CHECK(f.display.main_engine().control() == kPicture && f.display.second_engine().control() == kPicture);
  CHECK(f.io.unmodelled().empty());
}

TEST(display_each_engine_on_its_screen) {
  Fixture f;
  // Until a frame is done the screens are white.
  CHECK(f.display.top_screen()[0] == 0x3FFFF && f.display.bottom_screen()[0] == 0x3FFFF);
  f.io.write(0x04000000, 4, kPicture);
  f.io.write(0x04001000, 4, kPicture);
  f.frame();
  // The main engine is on the bottom screen unless the power register says
  // otherwise.
  CHECK(f.display.top_screen()[0] == kGreen && f.display.bottom_screen()[0] == kRed);
  CHECK(f.display.top_screen()[256 * 192 - 1] == kGreen && f.display.bottom_screen()[256 * 192 - 1] == kRed);
  f.io.write(Display::kPowerControl, 2, Display::kMainOnTop);
  f.frame();
  CHECK(f.display.top_screen()[0] == kRed && f.display.bottom_screen()[0] == kGreen);
  CHECK(f.display.statistics().frames == 2);
}

TEST(display_capture_of_the_main_engine_s_picture) {
  Fixture f;
  f.io.write(0x04000000, 4, kPicture);
  // Into bank B, 256 by 192. Six bits a part become five, with the top bit
  // set.
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 16);
  f.frame();
  CHECK(f.bank_b(0, 0) == (0x8000 | kRed15) && f.bank_b(255, 191) == (0x8000 | kRed15));
  CHECK(f.bank_a(0, 0) == 0);
  // The capture switches itself off when done.
  CHECK(f.io.read(Display::kCaptureControl, 4) == (3u << 20 | 1u << 16));
  CHECK(f.display.statistics().captures == 1);
  f.backdrop(false, kGreen15);
  f.frame();
  CHECK(f.bank_b(0, 0) == (0x8000 | kRed15));

  // Switched on in the middle of a frame, it is made during the next.
  for (unsigned line = 0; line < 100; ++line) {
    f.display.line_begins(line);
    f.display.line_done(line);
  }
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 16);
  for (unsigned line = 100; line < 192; ++line) {
    f.display.line_begins(line);
    f.display.line_done(line);
  }
  CHECK(f.bank_b(0, 150) == (0x8000 | kRed15) && (f.io.read(Display::kCaptureControl, 4) & Display::kCaptureOn) != 0);
  f.frame();
  CHECK(f.bank_b(0, 150) == (0x8000 | kGreen15) && (f.io.read(Display::kCaptureControl, 4) & Display::kCaptureOn) == 0);

  // A smaller capture, 128 by 128, to an offset of 0x8000 in bank A.
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 0u << 20 | 0u << 16 | 1u << 18);
  f.frame();
  CHECK(f.video.read(0x06808000, 2) == (0x8000 | kGreen15));
  CHECK(f.video.read(0x06808000 + (127 * 128 + 127) * 2, 2) == (0x8000 | kGreen15));
  CHECK(f.video.read(0x06808000 + 128 * 128 * 2, 2) == 0 && f.video.read(0x06800000, 2) == 0);

  // What is captured is the picture before the display mode and the master
  // brightness: here the display is off and faded to black.
  f.io.write(0x04000000, 4, 0);
  f.io.write(0x0400006C, 2, 2u << 14 | 16);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20);
  f.frame();
  CHECK(f.bank_a(10, 10) == (0x8000 | kGreen15) && f.display.bottom_screen()[0] == 0);

  // A bank that is not placed as plain memory gets nothing: bank B keeps
  // the green captured into it before.
  f.video.set_control(VideoMemory::kB, VideoMemory::kEnable | 1);
  f.backdrop(false, kRed15);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 16);
  f.frame();
  CHECK(f.video.contents(VideoMemory::kB)[0] == 0xE0 && f.video.contents(VideoMemory::kB)[1] == 0x83);
  CHECK((f.io.read(Display::kCaptureControl, 4) & Display::kCaptureOn) == 0);
  CHECK(f.video.statistics().writes_with_no_bank == 0);
}

TEST(display_capture_of_a_bank_and_of_two_sources_blended) {
  Fixture f;
  f.io.write(0x04000000, 4, kPicture);
  // The second source is the bank the main engine's control register names
  // (bank B here), from an offset.
  f.io.write(0x04000000, 4, kPicture | 1u << 18);
  f.video.write(0x06820000 + 0x8000, 2, 0x7C00);
  f.video.write(0x06820000 + 0x8000 + 2, 2, 0x03E0);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 29 | 1u << 26);
  f.frame();
  CHECK(f.bank_a(0, 0) == 0x7C00 && f.bank_a(1, 0) == 0x03E0 && f.bank_a(2, 0) == 0);
  // Blended: eight sixteenths of the engine's picture (red, 31) and of the
  // bank; then sixteen of each, held at 31.
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 2u << 29 | 1u << 26 | 8 | 8u << 8);
  f.frame();
  CHECK(f.bank_a(0, 0) == (0x8000 | 15 | 15u << 10) && f.bank_a(1, 0) == (0x8000 | 15 | 15u << 5) && f.bank_a(2, 0) == (0x8000 | 15));
  f.video.write(0x06820000 + 0x8000, 2, 0x001F);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 3u << 29 | 1u << 26 | 31 | 31u << 8);
  f.frame();
  CHECK(f.bank_a(0, 0) == (0x8000 | 31) && f.bank_a(1, 0) == (0x8000 | 31 | 31u << 5));
  // A second source fed from main memory is not there.
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 29 | 1u << 25);
  f.frame();
  CHECK(f.display.notes().size() == 1);
}

TEST(display_the_3d_picture) {
  Fixture f;
  f.send_triangle();
  // The main engine shows the 3D picture as background 0, over its backdrop.
  f.backdrop(false, kGreen15);
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  // The first frame: nothing has been handed over yet.
  f.frame();
  CHECK(f.display.statistics().pictures_3d == 0 && f.display.bottom_screen()[191 * 256] == kGreen);
  // The polygons were handed over at that frame's vertical blank; the next
  // frame shows them.
  f.frame();
  CHECK(f.display.statistics().pictures_3d == 1);
  CHECK(f.display.bottom_screen()[191 * 256] == rgb(63, 0, 0) && f.display.bottom_screen()[255] == kGreen);
  // Without another swap the picture stays and is not drawn again.
  f.frame();
  CHECK(f.display.statistics().pictures_3d == 1 && f.display.bottom_screen()[191 * 256] == rgb(63, 0, 0));
  // A capture can take the 3D picture alone, whatever the engine shows.
  f.io.write(0x04000000, 4, kPicture);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | Display::kCapture3DOnly);
  f.frame();
  CHECK(f.display.bottom_screen()[191 * 256] == kGreen);
  CHECK(f.bank_a(0, 191) == (0x8000 | 31) && f.bank_a(255, 0) == 0x8000);
  // An empty frame swapped in replaces it.
  f.io.write(0x04000540, 4, 0);
  f.frame();
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | Display::kCapture3DOnly);
  f.frame();
  CHECK(f.display.statistics().pictures_3d == 2 && f.bank_a(0, 191) == 0x8000);
}

TEST(display_marks_where_the_graphics_hardware_puts_the_3d_scene) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.backdrop(false, kGreen15);
  // Background 0 is 3D; blended over the backdrop (a second target).
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.io.write(0x04000050, 2, 1u | 1u << 13);
  f.frame();
  f.frame();
  // A scene was made in place of the picture; the screen is the backdrop,
  // marked everywhere, to be blended by the 3D pixel's alpha.
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kScene && f.display.statistics().pictures_3d == 1);
  CHECK(f.renderer.scene().serial == 1 && f.renderer.scene().vertices.size() == 3);
  const u32 pixel = f.display.bottom_screen()[191 * 256];
  CHECK((pixel & 0x3FFFF) == kGreen && (pixel & DisplayEngine::kMark3D) != 0);
  CHECK(((pixel >> DisplayEngine::kMarkEffectShift) & 3) == 1);
  // Darkened by the blend effect instead, by sixteenths.
  f.io.write(0x04000050, 2, 1u | 3u << 6);
  f.io.write(0x04000054, 2, 8);
  f.frame();
  const u32 darker = f.display.bottom_screen()[0];
  CHECK(((darker >> DisplayEngine::kMarkEffectShift) & 3) == 3 && ((darker >> DisplayEngine::kMarkFactorShift) & 31) == 8);
  // Not marked where a window hides background 0 (window 0, from x 0 to
  // 254, shows all but it). A capture is made by the graphics hardware too.
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8 | 1u << 13);
  f.io.write(0x04000040, 2, 0x00FF);
  f.io.write(0x04000044, 2, 0x00C0);
  f.io.write(0x04000048, 2, 0x3E);
  f.io.write(0x0400004A, 2, 0x3F);
  f.frame();
  CHECK((f.display.bottom_screen()[0] & DisplayEngine::kMark3D) == 0 &&
        (f.display.bottom_screen()[255] & DisplayEngine::kMark3D) != 0);
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.io.write(0x04000050, 2, 0);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20);
  f.frame();
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kScene && f.display.frame_capture() != 0);
  CHECK(f.display.statistics().frames_3d_processor == 0);
}

TEST(display_keeps_hardware_captures_and_finds_where_they_are_shown) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.frame();
  // The main engine's picture captured into bank B, by the graphics
  // hardware: a scene, and what the capture takes, marked where the scene
  // can go.
  f.io.write(Display::kPowerControl, 2, Display::kMainOnTop);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 3u << 20 | 1u << 16);
  f.frame();
  const std::uint64_t kept = f.display.frame_capture();
  CHECK(kept != 0 && f.display.frame_fill(0).fill == Display::Fill::kScene);
  CHECK((f.display.capture_source()[191 * 256] & DisplayEngine::kMark3D) != 0);
  const bd::nds::CaptureRecipe& recipe = f.display.capture_recipe();
  CHECK(recipe.source == 0 && !recipe.only_3d && recipe.width == 256 && recipe.height == 192 && recipe.write_start == 0);
  // The main engine shows bank B on the bottom screen: that capture, marked
  // everywhere it is the same, with the engine's master brightness.
  f.io.write(Display::kPowerControl, 2, 0);
  f.io.write(0x04000000, 4, 2u << 16 | 1u << 18);
  f.io.write(0x0400006C, 2, 0x4008);
  f.frame();
  const Display::ScreenFill& fill = f.display.frame_fill(1);
  CHECK(fill.fill == Display::Fill::kCapture && fill.capture == kept && fill.brightness == 0x4008);
  CHECK((f.display.bottom_screen()[191 * 256] & DisplayEngine::kMark3D) != 0);
  CHECK(f.display.frame_fill(0).fill == Display::Fill::kNone && f.display.frame_capture() == 0);
}

// 3D on both screens, the screens swapped in every frame, as the game's
// title does: each frame's scene is captured, and an engine shows a capture
// made before. Captures of the same picture are the same where the
// graphics hardware puts the 3D, but for the bits that tell them apart. A
// capture is written while its frame is drawn, so it is not what that frame
// shows.
TEST(display_a_screen_shows_the_capture_it_reads_when_the_screens_swap_every_frame) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  const u32 picture = kPicture | DisplayEngine::kBackground0Is3D | 1u << 8;
  f.io.write(0x04000000, 4, picture);
  const u32 capture = Display::kCaptureOn | 3u << 20;
  const auto frame = [&](bool main_on_top, u32 into_bank) {
    f.io.write(Display::kPowerControl, 2, main_on_top ? Display::kMainOnTop : 0);
    f.io.write(Display::kCaptureControl, 4, capture | into_bank << 16);
    f.frame();
    return f.display.frame_capture();
  };
  CHECK(frame(false, 0) != 0);
  const std::uint64_t into_b = frame(true, 1);
  const std::uint64_t into_a = frame(false, 0);
  REQUIRE(into_b != 0 && into_a != 0 && into_a != into_b);
  // Captured into bank A again, while the main engine shows bank B: the
  // capture two frames back, not the one now in bank A, nor this frame's.
  f.io.write(0x04000000, 4, (picture & ~kPicture) | 2u << 16 | 1u << 18);
  const std::uint64_t now = frame(true, 0);
  CHECK(now != 0 && now != into_a);
  const Display::ScreenFill& fill = f.display.frame_fill(0);
  CHECK(fill.fill == Display::Fill::kCapture && fill.capture == into_b);
  CHECK((f.display.top_screen()[191 * 256] & DisplayEngine::kMark3D) != 0);
  // Then bank A, with this frame's capture in bank B: the last frame's.
  f.io.write(0x04000000, 4, (picture & ~kPicture) | 2u << 16);
  frame(false, 1);
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kCapture && f.display.frame_fill(1).capture == now);
}

// Captures the graphics hardware makes with the 3D scene, whatever they
// take and wherever they go; in each, the processor draws no 3D picture.

TEST(display_hardware_captures_the_3d_picture_alone) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.frame();
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | Display::kCapture3DOnly | 3u << 20);
  f.frame();
  CHECK(f.display.frame_capture() != 0 && f.display.capture_recipe().only_3d);
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kScene);
  CHECK(f.display.statistics().frames_3d_processor == 0 && f.display.statistics().frames_3d_hardware == 1);
}

// A trail behind what moves, as the game's battles do: each frame's picture
// blended with the last capture, into the bank the main engine shows.
TEST(display_hardware_blends_a_capture_with_the_last_one) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, 2u << 16 | DisplayEngine::kBackground0Is3D | 1u << 8);
  const u32 blend = Display::kCaptureOn | 2u << 29 | 3u << 20 | 9u << 8 | 7u;
  f.io.write(Display::kCaptureControl, 4, blend);
  f.frame();
  const std::uint64_t first = f.display.frame_capture();
  const bd::nds::CaptureRecipe& recipe = f.display.capture_recipe();
  REQUIRE(first != 0);
  CHECK(recipe.source == 2 && recipe.weight_first == 7 && recipe.weight_second == 9);
  // Bank A held no capture of the graphics hardware: it goes as it is.
  CHECK(recipe.second_capture == 0 && recipe.second_pixels.size() == 0x10000);
  f.io.write(Display::kCaptureControl, 4, blend);
  f.frame();
  const std::uint64_t second = f.display.frame_capture();
  CHECK(second > first && recipe.second_capture == first && recipe.second_pixels.empty());
  // The screen shows the first, in bank A while the second was made.
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kCapture && f.display.frame_fill(1).capture == first);
  CHECK(f.display.statistics().frames_3d_processor == 0);
}

// 256 by 128, a quarter of the way into bank B: the screen showing bank B
// shows it from line 64, and the lines above are not it.
TEST(display_hardware_captures_other_sizes_and_places) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 2u << 20 | 1u << 18 | 1u << 16);
  f.frame();
  const std::uint64_t kept = f.display.frame_capture();
  REQUIRE(kept != 0);
  CHECK(f.display.capture_recipe().height == 128 && f.display.capture_recipe().write_start == 0x4000);
  f.io.write(0x04000000, 4, 2u << 16 | 1u << 18);
  f.frame();
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kCapture && f.display.frame_fill(1).capture == kept);
  CHECK((f.display.bottom_screen()[191 * 256] & DisplayEngine::kMark3D) != 0);
  CHECK((f.display.bottom_screen()[0] & DisplayEngine::kMark3D) == 0);
}

// The second source fed from main memory is not made (as without the
// graphics hardware); the scene is drawn all the same.
TEST(display_hardware_draws_the_scene_when_a_capture_reads_main_memory) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.frame();
  f.io.write(Display::kCaptureControl, 4, Display::kCaptureOn | 1u << 29 | Display::kCaptureFromMainMemory | 3u << 20);
  f.frame();
  CHECK(f.display.frame_fill(1).fill == Display::Fill::kScene && f.display.frame_capture() == 0);
  CHECK(f.display.statistics().frames_3d_processor == 0);
}

TEST(display_counts_the_frames_the_processor_draws_for_the_graphics_hardware) {
  Fixture f;
  f.display.set_hardware_3d(true);
  f.send_triangle();
  f.io.write(0x04000000, 4, kPicture | DisplayEngine::kBackground0Is3D | 1u << 8);
  f.frame();
  f.frame();
  f.display.hardware_3d_failed("the graphics hardware failed");
  f.frame();
  const Display::Statistics& s = f.display.statistics();
  CHECK(s.frames_3d_hardware == 1 && s.frames_3d_processor == 1);
  CHECK(f.display.fallbacks().size() == 1 && f.display.fallbacks().begin()->second == 1);
  // Not counted with the processor asked for.
  f.display.set_hardware_3d(false);
  f.frame();
  CHECK(s.frames_3d_processor == 1 && f.display.fallbacks().size() == 1);
}
