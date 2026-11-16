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
