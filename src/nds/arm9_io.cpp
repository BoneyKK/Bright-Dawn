#include "nds/arm9_io.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace bd::nds {
namespace {

std::uint32_t low_bytes(unsigned count) { return count >= 4 ? 0xFFFFFFFFu : ((1u << (8 * count)) - 1); }

}  // namespace

Arm9Io::Arm9Io() : stored_(kStoredSize) {}

void Arm9Io::add(std::uint32_t address, unsigned width, Read read, Write write) {
  assert(width == 1 || width == 2 || width == 4);
  assert(address % width == 0);
  std::uint32_t base = 0;
  assert(find(address, &base) == nullptr && find(address + width - 1, &base) == nullptr);
  (void)base;
  registers_[address] = Register{width, std::move(read), std::move(write)};
}

const Arm9Io::Register* Arm9Io::find(std::uint32_t address, std::uint32_t* base) const {
  auto it = registers_.upper_bound(address);
  if (it == registers_.begin()) return nullptr;
  --it;
  if (address - it->first >= it->second.width) return nullptr;
  *base = it->first;
  return &it->second;
}

std::uint8_t Arm9Io::load(std::uint32_t address) const {
  const std::uint32_t offset = address & 0x00FFFFFFu;
  return offset < kStoredSize ? stored_[offset] : 0;
}

void Arm9Io::store(std::uint32_t address, std::uint8_t value) {
  const std::uint32_t offset = address & 0x00FFFFFFu;
  if (offset < kStoredSize) stored_[offset] = value;
}

void Arm9Io::set(std::uint32_t address, unsigned size, std::uint32_t value) {
  for (unsigned i = 0; i < size; ++i) store(address + i, static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t Arm9Io::read(std::uint32_t address, unsigned size) {
  std::uint32_t result = 0;
  bool unmodelled = false;
  for (unsigned i = 0; i < size;) {
    std::uint32_t base = 0;
    const Register* reg = find(address + i, &base);
    if (reg == nullptr) {
      result |= static_cast<std::uint32_t>(load(address + i)) << (8 * i);
      unmodelled = true;
      ++i;
      continue;
    }
    // The part of this access that falls inside the register. The register is
    // read once, whatever part is wanted: reading can have an effect.
    const unsigned offset = address + i - base;
    const unsigned count = std::min(size - i, reg->width - offset);
    const std::uint32_t value = reg->read ? reg->read() : 0;
    result |= ((value >> (8 * offset)) & low_bytes(count)) << (8 * i);
    i += count;
  }
  if (unmodelled) {
    RegisterUse& use = unmodelled_[address];
    ++use.reads;
    use.size = size;
  }
  return result;
}

void Arm9Io::write(std::uint32_t address, unsigned size, std::uint32_t value) {
  bool unmodelled = false;
  for (unsigned i = 0; i < size;) {
    std::uint32_t base = 0;
    const Register* reg = find(address + i, &base);
    if (reg == nullptr) {
      store(address + i, static_cast<std::uint8_t>(value >> (8 * i)));
      unmodelled = true;
      ++i;
      continue;
    }
    const unsigned offset = address + i - base;
    const unsigned count = std::min(size - i, reg->width - offset);
    if (reg->write) {
      const std::uint32_t part = (value >> (8 * i)) & low_bytes(count);
      reg->write(part << (8 * offset), low_bytes(count) << (8 * offset));
    }
    i += count;
  }
  if (unmodelled) {
    RegisterUse& use = unmodelled_[address];
    ++use.writes;
    use.size = size;
    use.last_written = value;
  }
}

}  // namespace bd::nds
