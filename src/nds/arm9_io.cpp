#include "nds/arm9_io.h"

namespace bd::nds {

Arm9Io::Arm9Io() : stored_(kStoredSize) {}

std::uint32_t Arm9Io::load(std::uint32_t address, unsigned size) const {
  const std::uint32_t offset = address & 0x00FFFFFFu;
  if (offset > kStoredSize - size) return 0;
  std::uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(stored_[offset + i]) << (8 * i);
  return value;
}

void Arm9Io::store(std::uint32_t address, unsigned size, std::uint32_t value) {
  const std::uint32_t offset = address & 0x00FFFFFFu;
  if (offset > kStoredSize - size) return;
  for (unsigned i = 0; i < size; ++i) stored_[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void Arm9Io::set(std::uint32_t address, unsigned size, std::uint32_t value) { store(address, size, value); }

std::uint32_t Arm9Io::read(std::uint32_t address, unsigned size) {
  RegisterUse& use = unmodelled_[address];
  ++use.reads;
  use.size = size;
  return load(address, size);
}

void Arm9Io::write(std::uint32_t address, unsigned size, std::uint32_t value) {
  RegisterUse& use = unmodelled_[address];
  ++use.writes;
  use.size = size;
  use.last_written = value;
  store(address, size, value);
}

}  // namespace bd::nds
