#include "rom_builder.h"

#include <algorithm>

#include "core/checksum.h"

namespace bd_test {
namespace {

constexpr std::uint32_t kArm9RamAddress = 0x02000000;
constexpr std::uint32_t kArm7RamAddress = 0x02380000;
constexpr std::size_t kArm9Prefix = 0x100;  // Left uncompressed, like real start-up code.
constexpr std::size_t kModuleParamsOffset = 0x40;

struct Dir {
  std::string name;
  int parent = -1;
  std::vector<int> subdirs;
  std::vector<std::pair<std::string, std::size_t>> files;  // Name, index into spec.files.
};

void align(Bytes& image, std::size_t alignment) {
  while (image.size() % alignment != 0) image.push_back(0xFF);
}

void append(Bytes& image, const Bytes& data) { image.insert(image.end(), data.begin(), data.end()); }

void push_le16(Bytes& data, std::uint32_t value) {
  data.push_back(static_cast<std::uint8_t>(value));
  data.push_back(static_cast<std::uint8_t>(value >> 8));
}

void push_le32(Bytes& data, std::uint32_t value) {
  push_le16(data, value & 0xFFFF);
  push_le16(data, value >> 16);
}

}  // namespace

void put_le16(Bytes& data, std::size_t offset, std::uint32_t value) {
  data[offset] = static_cast<std::uint8_t>(value);
  data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_le32(Bytes& data, std::size_t offset, std::uint32_t value) {
  put_le16(data, offset, value & 0xFFFF);
  put_le16(data, offset + 2, value >> 16);
}

void fix_header_crc(Bytes& image) {
  put_le16(image, 0x15E, bd::crc16(bd::ByteSpan(image).first(0x15E)));
}

BuiltRom build_rom(const RomSpec& spec) {
  BuiltRom built;

  // --- ARM9 program ---
  Bytes arm9(kArm9Prefix, 0);
  for (std::size_t i = 0; i < arm9.size(); ++i) arm9[i] = static_cast<std::uint8_t>(i * 7 + 3);
  if (spec.decrypted_secure_area) {
    put_le32(arm9, 0, 0xE7FFDEFF);
    put_le32(arm9, 4, 0xE7FFDEFF);
  }
  if (spec.with_module_params) {
    std::size_t p = kModuleParamsOffset;
    for (std::uint32_t word : {0x02004000u, 0x02004010u, 0x02004020u, 0x02005000u, 0x02006000u,
                               0u, spec.sdk_version, 0x2106C0DEu, 0xDEC00621u}) {
      put_le32(arm9, p, word);
      p += 4;
    }
  }
  append(arm9, sample_data(0x1800, 9));
  built.arm9_plain = arm9;

  Bytes arm9_stored = arm9;
  if (spec.compress_arm9 && spec.with_module_params) {
    arm9_stored = blz_compress(arm9, kArm9Prefix);
    put_le32(arm9_stored, kModuleParamsOffset + 20,
             kArm9RamAddress + static_cast<std::uint32_t>(arm9_stored.size()));
    // The decompressed image keeps the marker, exactly as it would in memory.
    put_le32(built.arm9_plain, kModuleParamsOffset + 20,
             kArm9RamAddress + static_cast<std::uint32_t>(arm9_stored.size()));
  }

  const Bytes arm7 = sample_data(0x400, 7);

  // --- Directory tree ---
  std::vector<Dir> dirs(1);
  for (std::size_t i = 0; i < spec.files.size(); ++i) {
    const std::string& path = spec.files[i].first;
    int current = 0;
    std::size_t start = 0;
    for (;;) {
      const std::size_t slash = path.find('/', start);
      if (slash == std::string::npos) {
        dirs[current].files.emplace_back(path.substr(start), i);
        break;
      }
      const std::string name = path.substr(start, slash - start);
      int next = -1;
      for (int sub : dirs[current].subdirs) {
        if (dirs[sub].name == name) next = sub;
      }
      if (next < 0) {
        next = static_cast<int>(dirs.size());
        Dir dir;
        dir.name = name;
        dir.parent = current;
        dirs.push_back(dir);
        dirs[current].subdirs.push_back(next);
      }
      current = next;
      start = slash + 1;
    }
  }

  // File IDs: overlays first, then each directory's files as a contiguous run.
  const std::size_t overlay_count = spec.overlays.size();
  std::vector<Bytes> file_data;
  for (const OverlaySpec& overlay : spec.overlays)
    file_data.push_back(overlay.compressed ? blz_compress(overlay.data, 0) : overlay.data);
  std::vector<std::uint32_t> first_file(dirs.size());
  for (std::size_t d = 0; d < dirs.size(); ++d) {
    first_file[d] = static_cast<std::uint32_t>(file_data.size());
    for (const auto& file : dirs[d].files) file_data.push_back(spec.files[file.second].second);
  }

  // --- File name table ---
  Bytes fnt(dirs.size() * 8, 0);
  for (std::size_t d = 0; d < dirs.size(); ++d) {
    put_le32(fnt, d * 8, static_cast<std::uint32_t>(fnt.size()));
    put_le16(fnt, d * 8 + 4, first_file[d]);
    put_le16(fnt, d * 8 + 6,
             d == 0 ? static_cast<std::uint32_t>(dirs.size())
                    : 0xF000u + static_cast<std::uint32_t>(dirs[d].parent));
    for (const auto& file : dirs[d].files) {
      fnt.push_back(static_cast<std::uint8_t>(file.first.size()));
      fnt.insert(fnt.end(), file.first.begin(), file.first.end());
    }
    for (int sub : dirs[d].subdirs) {
      fnt.push_back(static_cast<std::uint8_t>(0x80 | dirs[sub].name.size()));
      fnt.insert(fnt.end(), dirs[sub].name.begin(), dirs[sub].name.end());
      push_le16(fnt, 0xF000u + static_cast<std::uint32_t>(sub));
    }
    fnt.push_back(0);
  }

  // --- Image layout ---
  Bytes& image = built.image;
  image.assign(0x4000, 0);
  const std::uint32_t arm9_offset = static_cast<std::uint32_t>(image.size());
  append(image, arm9_stored);
  align(image, 0x200);
  const std::uint32_t arm7_offset = static_cast<std::uint32_t>(image.size());
  append(image, arm7);
  align(image, 0x200);

  built.overlay_table_offset = static_cast<std::uint32_t>(image.size());
  for (std::size_t i = 0; i < overlay_count; ++i) {
    const OverlaySpec& overlay = spec.overlays[i];
    push_le32(image, static_cast<std::uint32_t>(i));
    push_le32(image, overlay.ram_address);
    push_le32(image, static_cast<std::uint32_t>(overlay.data.size()));
    push_le32(image, 0x100);  // bss size
    push_le32(image, overlay.ram_address);
    push_le32(image, overlay.ram_address + 4);
    push_le32(image, static_cast<std::uint32_t>(i));  // file ID
    push_le32(image, (overlay.compressed ? 0x01000000u : 0u) |
                         static_cast<std::uint32_t>(file_data[i].size()));
  }
  align(image, 0x200);

  built.fnt_offset = static_cast<std::uint32_t>(image.size());
  built.fnt_size = static_cast<std::uint32_t>(fnt.size());
  append(image, fnt);
  align(image, 0x200);

  built.fat_offset = static_cast<std::uint32_t>(image.size());
  built.fat_size = static_cast<std::uint32_t>(file_data.size() * 8);
  image.resize(image.size() + built.fat_size, 0);
  align(image, 0x200);
  for (std::size_t i = 0; i < file_data.size(); ++i) {
    const std::uint32_t start = static_cast<std::uint32_t>(image.size());
    append(image, file_data[i]);
    put_le32(image, built.fat_offset + i * 8, start);
    put_le32(image, built.fat_offset + i * 8 + 4, static_cast<std::uint32_t>(image.size()));
    align(image, 4);
  }
  align(image, 0x200);

  // --- Header ---
  std::copy_n(spec.title.begin(), std::min<std::size_t>(spec.title.size(), 12), image.begin());
  std::copy_n(spec.game_code.begin(), std::min<std::size_t>(spec.game_code.size(), 4),
              image.begin() + 0x0C);
  image[0x10] = '0';
  image[0x11] = '1';
  image[0x14] = 0;  // 128 KiB chip; irrelevant to the tests.
  image[0x1E] = spec.rom_version;
  put_le32(image, 0x20, arm9_offset);
  put_le32(image, 0x24, kArm9RamAddress + 0x800);
  put_le32(image, 0x28, kArm9RamAddress);
  put_le32(image, 0x2C, static_cast<std::uint32_t>(arm9_stored.size()));
  put_le32(image, 0x30, arm7_offset);
  put_le32(image, 0x34, kArm7RamAddress);
  put_le32(image, 0x38, kArm7RamAddress);
  put_le32(image, 0x3C, static_cast<std::uint32_t>(arm7.size()));
  put_le32(image, 0x40, built.fnt_offset);
  put_le32(image, 0x44, built.fnt_size);
  put_le32(image, 0x48, built.fat_offset);
  put_le32(image, 0x4C, built.fat_size);
  put_le32(image, 0x50, overlay_count == 0 ? 0 : built.overlay_table_offset);
  put_le32(image, 0x54, static_cast<std::uint32_t>(overlay_count * 32));
  put_le32(image, 0x80, static_cast<std::uint32_t>(image.size()));
  put_le32(image, 0x84, 0x4000);
  // Stand-in logo: arbitrary bytes with a matching checksum.
  for (std::size_t i = 0; i < 0x9C; ++i) image[0xC0 + i] = static_cast<std::uint8_t>(i * 13 + 1);
  put_le16(image, 0x15C, bd::crc16(bd::ByteSpan(image).subspan(0xC0, 0x9C)));
  fix_header_crc(image);
  return built;
}

RomSpec default_spec() {
  RomSpec spec;
  const Bytes model = [] {
    Bytes data = {'B', 'M', 'D', '0'};
    const Bytes body = sample_data(900, 21);
    data.insert(data.end(), body.begin(), body.end());
    return data;
  }();
  spec.files = {
      {"sound_data.sdat", [] {
         Bytes data = {'S', 'D', 'A', 'T'};
         const Bytes body = sample_data(2000, 3);
         data.insert(data.end(), body.begin(), body.end());
         return data;
       }()},
      {"readme.txt", Bytes{'h', 'e', 'l', 'l', 'o'}},
      {"data/model/hero.nsbmd", lz10_compress(model)},
      {"data/model/town.bin", sample_data(700, 5)},
      {"data/empty.dat", Bytes{}},
      {"data/font/main.nftr", [] {
         Bytes data = {'R', 'T', 'F', 'N'};
         const Bytes body = sample_data(300, 8);
         data.insert(data.end(), body.begin(), body.end());
         return data;
       }()},
      {"script/ev/0001.bin", sample_data(64, 11)},
  };
  spec.overlays = {
      OverlaySpec{sample_data(0x900, 31), true, 0x02100000},
      OverlaySpec{sample_data(0x500, 32), false, 0x02100000},
      OverlaySpec{sample_data(0x700, 33), true, 0x02180000},
  };
  return spec;
}

}  // namespace bd_test
