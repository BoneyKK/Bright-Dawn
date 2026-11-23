// `bdtool boot`: runs the game's start-up code and reports what it asked of
// the machine. A diagnostic for developing the runtime, not a way to play.
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/version.h"
#include "nds/console.h"
#include "nds/geometry_engine.h"
#include "nds/polygon_renderer.h"
#include "nds/program.h"
#include "rom/rom.h"
#include "translated_table.h"

namespace {

// This tool has no translated code linked in: everything is interpreted.
const TranslatedEntry kNoTranslations[] = {{0, nullptr, -1, nullptr, 0}};

const char* mode_name(std::uint32_t mode) {
  switch (mode & 0x1Fu) {
    case 0x10: return "User";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "Supervisor";
    case 0x17: return "Abort";
    case 0x1B: return "Undefined";
    case 0x1F: return "System";
    default: return "invalid";
  }
}

// The outlines of a 3D frame's polygons as an SVG picture of the screen, each
// in the colour of its first corner. Not a rendering: nothing is filled,
// textured or hidden behind anything.
bool write_wireframe(const std::filesystem::path& path, const bd::nds::GeometryEngine::Frame& frame) {
  std::FILE* file = std::fopen(path.string().c_str(), "w");
  if (file == nullptr) return false;
  std::fprintf(file,
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 256 192\" width=\"1024\" height=\"768\">\n"
               "<rect width=\"256\" height=\"192\" fill=\"#101018\"/>\n"
               "<g fill=\"none\" stroke-width=\"0.25\" stroke-linejoin=\"round\">\n");
  for (const bd::nds::GeometryEngine::Polygon& polygon : frame.polygons) {
    if (polygon.first + std::size_t{polygon.size} > frame.vertices.size()) continue;
    const bd::nds::GeometryEngine::Vertex* corner = &frame.vertices[polygon.first];
    // Six bits a part made eight.
    const auto part = [&](unsigned shift) {
      const unsigned six = (corner[0].colour >> shift) & 0x3F;
      return six << 2 | six >> 4;
    };
    std::fprintf(file, "<polygon stroke=\"#%02X%02X%02X\" points=\"", part(0), part(6), part(12));
    for (unsigned i = 0; i < polygon.size; ++i) {
      // A strip's uncut four-cornered polygon is stored in the strip's order.
      const unsigned at = polygon.strip_order && i >= 2 ? 5 - i : i;
      std::fprintf(file, "%s%d,%d", i == 0 ? "" : " ", static_cast<int>(corner[at].x), static_cast<int>(corner[at].y));
    }
    std::fprintf(file, "\"/>\n");
  }
  std::fprintf(file, "</g>\n</svg>\n");
  return std::fclose(file) == 0;
}

// Pixels of six bits a part as a BMP file, 24 bits a pixel, 256 across. With
// `with_alpha`, pixels whose alpha (from bit 18) is zero are black: nothing
// was drawn on them.
bool write_picture(const std::filesystem::path& path, const std::vector<std::uint32_t>& image, bool with_alpha = true) {
  constexpr int width = bd::nds::PolygonRenderer::kWidth;
  const int height = static_cast<int>(image.size() / width);
  if (height == 0 || image.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) return false;
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) return false;
  const std::uint32_t data_size = static_cast<std::uint32_t>(width * height * 3);
  unsigned char header[54] = {'B', 'M'};
  const auto put = [&](int at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) header[at + i] = static_cast<unsigned char>(value >> (8 * i));
  };
  put(2, 54 + data_size);
  put(10, 54);
  put(14, 40);
  put(18, static_cast<std::uint32_t>(width));
  put(22, static_cast<std::uint32_t>(height));
  header[26] = 1;   // One plane,
  header[28] = 24;  // 24 bits a pixel.
  put(34, data_size);
  std::fwrite(header, 1, sizeof header, file);
  // Lines from the bottom up, blue first. Six bits a part made eight.
  for (int y = height - 1; y >= 0; --y) {
    for (int x = 0; x < width; ++x) {
      const std::uint32_t pixel = image[static_cast<std::size_t>(y * width + x)];
      const bool drawn = !with_alpha || ((pixel >> 18) & 0x3F) != 0;
      for (unsigned shift : {12u, 6u, 0u}) {
        const std::uint32_t six = drawn ? (pixel >> shift) & 0x3F : 0;
        std::fputc(static_cast<int>(six << 2 | six >> 4), file);
      }
    }
  }
  return std::fclose(file) == 0;
}

}  // namespace

int cmd_boot(const std::filesystem::path& source, std::uint64_t instruction_limit, std::uint64_t frame_limit,
             const std::filesystem::path& wireframe, const std::filesystem::path& picture,
             const std::filesystem::path& screens) {
  using namespace bd;
  std::printf("bdtool %s\n", BD_VERSION);

  std::error_code ec;
  const bool is_directory = std::filesystem::is_directory(source, ec);
  Result<nds::Program> loaded = Error{ErrorCode::Io, "not loaded"};
  Result<rom::Rom> rom = Error{ErrorCode::Io, "no ROM"};
  if (is_directory) {
    loaded = nds::program_from_directory(source);
  } else {
    rom = rom::Rom::open(source);
    if (!rom) {
      std::fprintf(stderr, "error: %s\n", rom.error().message.c_str());
      return 1;
    }
    loaded = nds::program_from_rom(rom.value());
  }
  if (!loaded) {
    std::fprintf(stderr, "error: %s\n", loaded.error().message.c_str());
    return 1;
  }
  const nds::Program& program = loaded.value();

  nds::Console console(program, kNoTranslations);
  // With a ROM there is a cartridge to read files from; with only extracted
  // program code the slot is empty.
  if (rom) console.insert_cartridge(rom.value().bytes());
  if (auto s = console.direct_boot(); !s) {
    std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
    return 1;
  }
  const nds::RunResult result = console.run(instruction_limit, frame_limit);

  std::printf("Boot trace. Every instruction is interpreted; this tool contains no translated code.\n");
  std::printf("Program: ARM9 %zu bytes at 0x%08" PRIX32 ", entry 0x%08" PRIX32 ", %zu overlays, header %s\n",
              program.arm9.size(), program.arm9_load_address, program.arm9_entry, program.overlays.size(),
              program.header.empty() ? "not available" : "available");

  const auto& stats = console.machine().statistics();
  std::printf("Instructions interpreted: %" PRIu64 "\n", stats.interpreted_instructions);
  switch (result.reason) {
    case nds::StopReason::Returned:
      std::printf("Stopped: the program returned to the reset vector\n");
      break;
    case nds::StopReason::InstructionLimit:
      std::printf("Stopped: instruction limit reached, at 0x%08" PRIX32 "\n", result.address);
      break;
    case nds::StopReason::FrameLimit:
      std::printf("Stopped: frame limit reached\n");
      break;
    case nds::StopReason::Asked:
      std::printf("Stopped: asked to\n");
      break;
    case nds::StopReason::Reset:
      std::printf("Stopped: the program asked for a reset\n");
      break;
    case nds::StopReason::Unhandled:
      std::printf("Stopped: at 0x%08" PRIX32 ": %s\n", result.address, result.detail.c_str());
      break;
  }

  const double seconds = static_cast<double>(console.timeline().now()) / static_cast<double>(nds::Timeline::kCyclesPerSecond);
  std::printf("Console time passed: %.3f s, %" PRIu64 " frames; the processor slept %" PRIu64
              " times and took %" PRIu64 " interrupts\n",
              seconds, console.video_timing().frames(), console.sleeps(), stats.interrupts_taken);

  const ds::Cpu& c = console.machine().cpu();
  const std::uint32_t status = ds::get_cpsr(const_cast<ds::Cpu&>(c));
  std::printf("\nProcessor: %s mode, status 0x%08" PRIX32 "\n", mode_name(status), status);
  for (int i = 0; i < 15; ++i)
    std::printf("  r%-2d 0x%08" PRIX32 "%s", i, c.r[i], (i % 4 == 3 || i == 14) ? "\n" : "");

  nds::SystemControl& cp = console.system_control();
  std::printf("\nSystem coprocessor: control 0x%08" PRIX32 ", data memory register 0x%08" PRIX32
              ", instruction memory register 0x%08" PRIX32 "\n",
              cp.control(), cp.read(9, 1, 0), cp.read(9, 1, 1));
  for (const auto& [key, count] : cp.unknown_reads())
    std::printf("  unknown register c%u,c%u,%u read %" PRIu64 " times\n", key >> 8, (key >> 4) & 15, key & 15, count);
  for (const auto& [key, count] : cp.unknown_writes())
    std::printf("  unknown register c%u,c%u,%u written %" PRIu64 " times\n", key >> 8, (key >> 4) & 15, key & 15, count);

  std::printf("\nBIOS calls: %s\n", console.software_interrupts().empty() ? "none" : "");
  for (const auto& [number, count] : console.software_interrupts()) {
    const char* name = nds::Bios::name(number);
    std::printf("  0x%02" PRIX32 " %-16s %" PRIu64 "\n", number, name ? name : "(unknown)", count);
  }

  nds::Interrupts& irq = console.interrupts();
  std::printf("\nInterrupts: master switch %s, enabled 0x%08" PRIX32 ", requested 0x%08" PRIX32 "\n",
              irq.master() ? "on" : "off", irq.enabled(), irq.requested());
  std::printf("Timer overflows: %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n", console.timers().overflows(0),
              console.timers().overflows(1), console.timers().overflows(2), console.timers().overflows(3));

  nds::SecondProcessor& second = console.second_processor();
  std::printf("\nSecond processor: start-up handshake %s\n", second.handshake_done() ? "completed" : "not completed");
  if (!second.words_by_service().empty()) std::printf("  words sent to it, by service:\n");
  for (const auto& [service, count] : second.words_by_service()) {
    const auto unanswered = second.unanswered_by_service().find(service);
    std::printf("    service %2u: %" PRIu64 "%s\n", service, count,
                unanswered != second.unanswered_by_service().end() ? ", which nothing answers yet" : "");
  }
  std::printf("  words it sent back: %" PRIu64 "\n", second.words_answered());
  for (const auto& [what, count] : second.not_carried_out())
    std::printf("  not carried out and not answered, %" PRIu64 " times: %s\n", count, what.c_str());
  if (!second.first_words().empty()) {
    std::printf("  the first %zu, in order:", second.first_words().size());
    for (std::size_t i = 0; i < second.first_words().size(); ++i)
      std::printf("%s0x%08" PRIX32, i % 6 == 0 ? "\n    " : " ", second.first_words()[i]);
    std::printf("\n");
  }

  // Sound: the stand-in for the second processor's sound program, and the
  // hardware it drives.
  const nds::SoundDriver::Statistics& driver = console.sound_driver().statistics();
  std::printf("\nSound: %" PRIu64 " command lists received, %" PRIu64 " worked through", driver.lists_received,
              driver.lists_finished);
  if (driver.lists_dropped != 0) std::printf(", %" PRIu64 " dropped because eight were waiting", driver.lists_dropped);
  std::printf("; %" PRIu64 " alarm firings, %" PRIu64 " reported to the program\n", driver.alarms_fired,
              driver.alarm_words_sent);
  for (std::uint32_t number = 0; number < nds::SoundDriver::kCommands; ++number) {
    if (driver.commands[number] != 0)
      std::printf("  command 0x%02" PRIX32 ", %s: %" PRIu64 "\n", number, nds::SoundDriver::command_name(number),
                  driver.commands[number]);
  }
  for (const auto& [what, count] : console.sound_driver().not_carried_out())
    std::printf("  not carried out, %" PRIu64 " times: %s\n", count, what.c_str());
  const nds::SoundHardware::Statistics& sound = console.sound_hardware().statistics();
  std::printf("  hardware: %" PRIu64 " output samples, %" PRIu64 " of them not silent; %" PRIu64
              " channel starts, %" PRIu64 " channels played to their end; %" PRIu64 " bytes captured\n",
              sound.samples, sound.samples_with_sound, sound.channels_started, sound.channels_ended,
              sound.bytes_captured);
  for (const auto& [what, count] : console.sound_hardware().notes())
    std::printf("  not modelled, %" PRIu64 " times: %s\n", count, what.c_str());

  // The 3D engine's command queue.
  const nds::GeometryEngine::Statistics& geometry = console.geometry().statistics();
  std::printf("\n3D engine: %" PRIu64 " swaps; at most %" PRIu64 " entries queued; the processor waited for room %" PRIu64
              " times\n",
              geometry.swaps, geometry.most_queued, geometry.waits_for_room);
  std::printf("  status now 0x%08" PRIX32 "; commands carried out, by number:\n", console.geometry().status());
  for (unsigned number = 0; number < 256; ++number) {
    if (geometry.commands[number] != 0)
      std::printf("    0x%02X %-36s %" PRIu64 "\n", number, nds::GeometryEngine::command_name(number),
                  geometry.commands[number]);
  }
  if (geometry.unknown_commands != 0)
    std::printf("  of those, numbers that are no command: %" PRIu64 "\n", geometry.unknown_commands);
  std::printf("  %" PRIu64 " vertices sent; %" PRIu64 " polygons put into frames, %" PRIu64 " of them cut to the view\n",
              geometry.vertices, geometry.polygons, geometry.polygons_cut);
  std::printf("  polygons dropped: %" PRIu64 " facing away, %" PRIu64 " out of view, %" PRIu64 " past the far side\n",
              geometry.polygons_facing_away, geometry.polygons_out_of_view, geometry.polygons_past_far_side);
  std::printf("  the fullest frame had %" PRIu64 " vertices (of %zu) and %" PRIu64 " polygons (of %zu); no room for %" PRIu64
              " vertices and %" PRIu64 " polygons\n",
              geometry.most_vertices, nds::GeometryEngine::kMostVertices, geometry.most_polygons,
              nds::GeometryEngine::kMostPolygons, geometry.vertices_without_room, geometry.polygons_without_room);
  std::printf("  the frame last handed over: %zu vertices, %zu polygons\n", console.geometry().finished().vertices.size(),
              console.geometry().finished().polygons.size());
  std::printf("  drawing control 0x%04" PRIX32 ", clear colour 0x%08" PRIX32 ", clear depth 0x%04" PRIX32 "\n",
              console.polygon_renderer().control(), console.polygon_renderer().clear_colour(),
              console.polygon_renderer().clear_depth());
  if (!picture.empty()) {
    const nds::PolygonRenderer::Image& image =
        console.polygon_renderer().draw(console.geometry().finished(), console.bus().video_memory());
    if (write_picture(picture, image))
      std::printf("  that frame's 3D layer drawn and written to %s\n", picture.string().c_str());
    else
      std::fprintf(stderr, "error: cannot write %s\n", picture.string().c_str());
  }
  for (const auto& [what, count] : console.polygon_renderer().notes())
    std::printf("  not carried out when drawing: %s (%" PRIu64 " frames)\n", what.c_str(), count);

  // The display.
  nds::Display& display = console.display();
  std::printf("\nDisplay: %" PRIu64 " frames shown, %" PRIu64 " 3D pictures drawn, %" PRIu64 " display captures made\n",
              display.statistics().frames, display.statistics().pictures_3d, display.statistics().captures);
  std::printf("  main engine control 0x%08" PRIX32 ", second engine control 0x%08" PRIX32 "\n", display.main_engine().control(),
              display.second_engine().control());
  std::printf("  capture control 0x%08" PRIX32 ", power control 0x%04" PRIX32 " (the main engine is on the %s screen)\n",
              display.capture_control(), display.power_control(),
              (display.power_control() & nds::Display::kMainOnTop) != 0 ? "top" : "bottom");
  for (const auto* notes : {&display.main_engine().notes(), &display.second_engine().notes(), &display.notes()})
    for (const auto& [what, count] : *notes)
      std::printf("  not carried out: %s (%" PRIu64 " lines)\n", what.c_str(), count);
  if (!screens.empty()) {
    std::vector<std::uint32_t> both = display.top_screen();
    both.insert(both.end(), display.bottom_screen().begin(), display.bottom_screen().end());
    if (write_picture(screens, both, false))
      std::printf("  both screens written to %s\n", screens.string().c_str());
    else
      std::fprintf(stderr, "error: cannot write %s\n", screens.string().c_str());
  }
  if (!wireframe.empty()) {
    if (write_wireframe(wireframe, console.geometry().finished()))
      std::printf("  outlines of that frame's polygons written to %s\n", wireframe.string().c_str());
    else
      std::fprintf(stderr, "error: cannot write %s\n", wireframe.string().c_str());
  }

  nds::VideoMemory& video = console.bus().video_memory();
  std::printf("\nVideo memory: bank controls A to I, as last set:");
  for (unsigned bank = 0; bank < nds::VideoMemory::kBanks; ++bank)
    std::printf(" 0x%02X", video.control(static_cast<nds::VideoMemory::Bank>(bank)));
  std::printf("\n  accesses where no bank was: %" PRIu64 " reads, %" PRIu64 " writes\n",
              video.statistics().reads_with_no_bank, video.statistics().writes_with_no_bank);

  const nds::SaveMemory::Statistics& save = console.save_memory().statistics();
  std::printf("\nSave memory (blank at the start): %" PRIu64 " bytes read, %" PRIu64 " bytes written, %" PRIu64
              " sectors erased; %s\n",
              save.bytes_read, save.bytes_written, save.sectors_erased,
              console.save_memory().changed() ? "contents changed" : "contents unchanged");

  const nds::Dma::Statistics& dma = console.dma().statistics();
  std::printf("\nDMA: %" PRIu64 " transfers performed, %" PRIu64 " units moved\n", dma.transfers, dma.units);
  for (int mode = 1; mode < 8; ++mode) {
    if (dma.waiting_for_event[mode] == 0) continue;
    if (mode == static_cast<int>(nds::Dma::kStartCartridge)) {
      std::printf("  %" PRIu64 " started in mode %d, fed by the cartridge\n", dma.waiting_for_event[mode], mode);
    } else if (mode == static_cast<int>(nds::Dma::kStartVBlank)) {
      std::printf("  %" PRIu64 " started in mode %d, at the vertical blank\n", dma.waiting_for_event[mode], mode);
    } else if (mode == static_cast<int>(nds::Dma::kStartGeometry)) {
      std::printf("  %" PRIu64 " started in mode %d, feeding the 3D engine's queue\n", dma.waiting_for_event[mode],
                  mode);
    } else if (mode == static_cast<int>(nds::Dma::kStartHBlank)) {
      std::printf("  %" PRIu64 " started in mode %d, at the horizontal blank of each visible line\n",
                  dma.waiting_for_event[mode], mode);
    } else {
      std::printf("  %" PRIu64 " started in mode %d, which waits for an event that never comes yet\n",
                  dma.waiting_for_event[mode], mode);
    }
  }

  const nds::Cartridge::Statistics& card = console.cartridge().statistics();
  std::printf("\nCartridge: %s\n", console.cartridge().inserted() ? "inserted" : "none (started from extracted code)");
  std::printf("  data reads: %" PRIu64 ", %" PRIu64 " bytes", card.data_reads, card.data_bytes);
  if (card.data_bytes != 0)
    std::printf(", between 0x%08" PRIX32 " and 0x%08" PRIX32, card.lowest_address, card.highest_address);
  std::printf("\n  chip identifier reads: %" PRIu64 "\n", card.chip_id_reads);
  if (card.reads_without_cartridge != 0)
    std::printf("  data reads with no cartridge to answer: %" PRIu64 "\n", card.reads_without_cartridge);
  if (card.reads_beyond_the_end != 0)
    std::printf("  words read beyond the end of the cartridge: %" PRIu64 "\n", card.reads_beyond_the_end);
  for (const auto& [command, count] : card.other_commands)
    std::printf("  command 0x%02X, which is not modelled: %" PRIu64 "\n", command, count);

  const auto& registers = console.io().unmodelled();
  std::printf("\nHardware registers used that have no model: %zu\n", registers.size());
  if (!registers.empty()) std::printf("  address     width  reads      writes     last value written\n");
  for (const auto& [address, use] : registers) {
    std::printf("  0x%08" PRIX32 "  %u      %-10" PRIu64 " %-10" PRIu64 " ", address, use.size, use.reads, use.writes);
    if (use.writes != 0) std::printf("0x%0*" PRIX32, static_cast<int>(use.size * 2), use.last_written);
    std::printf("\n");
  }

  const auto& bus = console.bus().statistics();
  std::printf("\nMemory accessed where nothing is mapped: %s\n",
              bus.unmapped_reads.empty() && bus.unmapped_writes.empty() ? "none" : "");
  for (const auto& [block, count] : bus.unmapped_reads)
    std::printf("  0x%04" PRIX32 "0000: %" PRIu64 " reads\n", block, count);
  for (const auto& [block, count] : bus.unmapped_writes)
    std::printf("  0x%04" PRIX32 "0000: %" PRIu64 " writes\n", block, count);
  return 0;
}
