#include "nds/arm9_bus.h"

#include <cstring>

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

constexpr u32 kSharedRamSize = 32 * 1024;
constexpr u32 kPaletteSize = 2 * 1024;
constexpr u32 kSpriteSize = 2 * 1024;
// Video memory is really several banks that the program maps where it wants
// them. Until that is modelled the whole address range is backed by plain
// memory, so that what the program writes can at least be read back.
constexpr u32 kVideoSize = 16 * 1024 * 1024;

}  // namespace

Arm9Bus::Arm9Bus(Io& io)
    : io_(io),
      main_ram_(kMainRamSize),
      itcm_(kItcmSize),
      dtcm_(kDtcmSize),
      shared_ram_(kSharedRamSize),
      palette_(kPaletteSize),
      video_(kVideoSize),
      sprites_(kSpriteSize) {}

void Arm9Bus::map_itcm(bool readable, bool writable, std::uint64_t size) {
  itcm_map_ = FastMemory{readable, writable, 0, size};
}

void Arm9Bus::map_dtcm(bool readable, bool writable, u32 base, std::uint64_t size) {
  dtcm_map_ = FastMemory{readable, writable, base, size};
}

bool Arm9Bus::load(u32 address, const std::uint8_t* data, std::size_t size) {
  if (address < 0x02000000u || address - 0x02000000u > kMainRamSize || size > kMainRamSize - (address - 0x02000000u))
    return false;
  std::memcpy(main_ram_.data() + (address - 0x02000000u), data, size);
  return true;
}

std::uint8_t* Arm9Bus::locate(u32 address, bool write) {
  if (itcm_map_.claims(address, write)) return itcm_.data() + (address & (kItcmSize - 1));
  if (dtcm_map_.claims(address, write)) return dtcm_.data() + ((address - dtcm_map_.base) & (kDtcmSize - 1));
  // Every region below is a multiple of four bytes long and wide accesses are
  // aligned, so an access never runs off the end of one.
  switch (address >> 24) {
    case 0x02: return main_ram_.data() + (address & (kMainRamSize - 1));
    case 0x03: return shared_ram_.data() + (address & (kSharedRamSize - 1));
    case 0x05: return palette_.data() + (address & (kPaletteSize - 1));
    case 0x06: return video_.data() + (address & (kVideoSize - 1));
    case 0x07: return sprites_.data() + (address & (kSpriteSize - 1));
    default: return nullptr;
  }
}

bool Arm9Bus::is_io(u32 address, bool write) const {
  return (address >> 24) == 0x04 && !itcm_map_.claims(address, write) && !dtcm_map_.claims(address, write);
}

u32 Arm9Bus::read(u32 address, unsigned size) {
  if (is_io(address, false)) return io_.read(address, size);
  const std::uint8_t* p = locate(address, false);
  if (p == nullptr) {
    ++statistics_.unmapped_reads[address >> 16];
    return 0;
  }
  u32 value = 0;
  std::memcpy(&value, p, size);  // The host is little-endian, like the guest.
  return value;
}

void Arm9Bus::write(u32 address, unsigned size, u32 value) {
  if (is_io(address, true)) {
    io_.write(address, size, value);
    return;
  }
  std::uint8_t* p = locate(address, true);
  if (p == nullptr) {
    ++statistics_.unmapped_writes[address >> 16];
    return;
  }
  std::memcpy(p, &value, size);
}

std::uint8_t Arm9Bus::read8(u32 address) { return static_cast<std::uint8_t>(read(address, 1)); }
std::uint16_t Arm9Bus::read16(u32 address) { return static_cast<std::uint16_t>(read(address & ~1u, 2)); }
std::uint32_t Arm9Bus::read32(u32 address) { return read(address & ~3u, 4); }
void Arm9Bus::write8(u32 address, std::uint8_t value) { write(address, 1, value); }
void Arm9Bus::write16(u32 address, std::uint16_t value) { write(address & ~1u, 2, value); }
void Arm9Bus::write32(u32 address, std::uint32_t value) { write(address & ~3u, 4, value); }

}  // namespace bd::nds
