#include "nds/scene_dump.h"

#include <cstring>
#include <fstream>
#include <type_traits>

namespace bd::nds {

namespace {

constexpr char kMagic[8] = {'B', 'D', 'S', 'C', 'E', 'N', 'E', '1'};

struct Writer {
  std::ofstream out;
  template <typename T>
  void put(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
  }
  template <typename T>
  void put_all(const std::vector<T>& v) {
    put(static_cast<std::uint64_t>(v.size()));
    if (!v.empty()) out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
  }
};

struct Reader {
  std::ifstream in;
  template <typename T>
  bool get(T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof v));
  }
  template <typename T>
  bool get_all(std::vector<T>& v, std::uint64_t most) {
    std::uint64_t n = 0;
    if (!get(n) || n > most) return false;
    v.resize(static_cast<std::size_t>(n));
    return n == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T))));
  }
};

}  // namespace

bool write_scene_dump(const std::filesystem::path& path, const SceneDump& dump, std::string& error) {
  Writer w{std::ofstream(path, std::ios::binary)};
  if (!w.out) {
    error = "cannot write " + path.string();
    return false;
  }
  const PolygonRenderer::Inputs& in = dump.inputs;
  w.out.write(kMagic, sizeof kMagic);
  w.put(static_cast<std::uint32_t>(dump.edge_marking));
  w.put(static_cast<std::uint32_t>(dump.fine_positions ? 1 : 0));
  w.put(static_cast<std::int32_t>(dump.scale));
  for (const std::uint32_t r : {in.control, in.alpha_reference, in.clear_colour, in.clear_depth, in.clear_image_offset,
                                in.fog_colour, in.fog_offset})
    w.put(r);
  w.put(in.edge_colours);
  w.put(in.fog_table);
  w.put(in.toon_table);
  w.put(static_cast<std::uint32_t>(in.frame.depth_is_w ? 1 : 0));
  w.put(in.frame.swap_parameter);
  w.put_all(in.frame.vertices);
  w.put_all(in.frame.polygons);
  w.put_all(in.textures);
  w.put_all(in.palettes);
  for (int i = 0; i < 2; ++i) {
    w.put(dump.fills[static_cast<std::size_t>(i)]);
    w.put(dump.brightness[static_cast<std::size_t>(i)]);
    w.put_all(dump.screens[static_cast<std::size_t>(i)]);
  }
  w.out.flush();
  if (!w.out) {
    error = "cannot write " + path.string();
    return false;
  }
  return true;
}

bool read_scene_dump(const std::filesystem::path& path, SceneDump& dump, std::string& error) {
  Reader r{std::ifstream(path, std::ios::binary)};
  char magic[sizeof kMagic] = {};
  if (!r.in || !r.in.read(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
    error = path.string() + " is not a 3D scene dump";
    return false;
  }
  PolygonRenderer::Inputs& in = dump.inputs;
  std::uint32_t marking = 0, fine = 0, depth_is_w = 0;
  std::int32_t scale = 1;
  bool ok = r.get(marking) && r.get(fine) && r.get(scale);
  for (std::uint32_t* reg : {&in.control, &in.alpha_reference, &in.clear_colour, &in.clear_depth, &in.clear_image_offset,
                             &in.fog_colour, &in.fog_offset})
    ok = ok && r.get(*reg);
  ok = ok && r.get(in.edge_colours) && r.get(in.fog_table) && r.get(in.toon_table) && r.get(depth_is_w) &&
       r.get(in.frame.swap_parameter) && r.get_all(in.frame.vertices, 1u << 16) && r.get_all(in.frame.polygons, 1u << 16) &&
       r.get_all(in.textures, VideoMemory::kTextureSize) && r.get_all(in.palettes, VideoMemory::kTexturePaletteSize);
  for (int i = 0; i < 2 && ok; ++i)
    ok = r.get(dump.fills[static_cast<std::size_t>(i)]) && r.get(dump.brightness[static_cast<std::size_t>(i)]) &&
         r.get_all(dump.screens[static_cast<std::size_t>(i)], 256 * 192);
  if (!ok || marking > 2) {
    error = path.string() + " is cut short or damaged";
    return false;
  }
  dump.edge_marking = static_cast<EdgeMarking>(marking);
  dump.fine_positions = fine != 0;
  dump.scale = scale;
  in.frame.depth_is_w = depth_is_w != 0;
  return true;
}

void restore_scene_inputs(const PolygonRenderer::Inputs& in, Arm9Io& io, VideoMemory& video) {
  io.write(PolygonRenderer::kControl, 2, in.control & 0x4FFF);
  io.write(PolygonRenderer::kAlphaTestReference, 2, in.alpha_reference);
  io.write(PolygonRenderer::kClearColour, 4, in.clear_colour);
  io.write(PolygonRenderer::kClearDepth, 2, in.clear_depth);
  io.write(PolygonRenderer::kClearImageOffset, 2, in.clear_image_offset);
  io.write(PolygonRenderer::kFogColour, 4, in.fog_colour);
  io.write(PolygonRenderer::kFogOffset, 2, in.fog_offset);
  for (std::uint32_t i = 0; i < 8; ++i) io.write(PolygonRenderer::kEdgeColours + 2 * i, 2, in.edge_colours[i]);
  for (std::uint32_t i = 0; i < 32; ++i) io.write(PolygonRenderer::kFogTable + i, 1, in.fog_table[i]);
  for (std::uint32_t i = 0; i < 32; ++i) io.write(PolygonRenderer::kToonTable + 2 * i, 2, in.toon_table[i]);

  // Each bank as plain memory first, filled, then handed to the 3D engine.
  // A slot with no bank then stays without one.
  const auto present = [](const std::vector<std::int16_t>& memory, std::uint32_t from, std::uint32_t size) {
    for (std::uint32_t a = from; a < from + size && a < memory.size(); ++a)
      if (memory[a] >= 0) return true;
    return false;
  };
  const auto fill = [&](VideoMemory::Bank bank, std::uint32_t plain, const std::vector<std::int16_t>& memory,
                        std::uint32_t from, std::uint32_t size, std::uint8_t control) {
    video.set_control(bank, VideoMemory::kEnable);
    for (std::uint32_t a = 0; a < size && from + a < memory.size(); ++a)
      video.write(plain + a, 1, static_cast<std::uint32_t>(std::max<std::int16_t>(memory[from + a], 0)));
    video.set_control(bank, control);
  };
  constexpr std::uint32_t kSlot = 0x20000, kPaletteSlot = 0x4000;
  const VideoMemory::Bank texture_banks[4] = {VideoMemory::kA, VideoMemory::kB, VideoMemory::kC, VideoMemory::kD};
  for (std::uint32_t slot = 0; slot < 4; ++slot)
    if (present(in.textures, slot * kSlot, kSlot))
      fill(texture_banks[slot], 0x06800000 + slot * kSlot, in.textures, slot * kSlot, kSlot,
           static_cast<std::uint8_t>(VideoMemory::kEnable | 3 | slot << 3));
  if (present(in.palettes, 0, 4 * kPaletteSlot))
    fill(VideoMemory::kE, 0x06880000, in.palettes, 0, 4 * kPaletteSlot, VideoMemory::kEnable | 3);
  // F and G at palette slots 4 and 5: offsets 2 and 3.
  if (present(in.palettes, 4 * kPaletteSlot, kPaletteSlot))
    fill(VideoMemory::kF, 0x06890000, in.palettes, 4 * kPaletteSlot, kPaletteSlot, VideoMemory::kEnable | 3 | 2 << 3);
  if (present(in.palettes, 5 * kPaletteSlot, kPaletteSlot))
    fill(VideoMemory::kG, 0x06894000, in.palettes, 5 * kPaletteSlot, kPaletteSlot, VideoMemory::kEnable | 3 | 3 << 3);
}

}  // namespace bd::nds
