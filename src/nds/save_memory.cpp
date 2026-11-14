#include "nds/save_memory.h"

#include <algorithm>

namespace bd::nds {

SaveMemory::SaveMemory() : bytes_(kSize, 0xFF) {}

std::uint8_t SaveMemory::read(std::uint32_t address) {
  ++statistics_.bytes_read;
  return bytes_[address % kSize];
}

void SaveMemory::page_write(std::uint32_t address, const std::uint8_t* data, std::size_t size) {
  const std::size_t page = (address % kSize) & ~(kPageSize - 1);
  std::size_t in_page = address & (kPageSize - 1);
  for (std::size_t i = 0; i < size; ++i) {
    std::uint8_t& byte = bytes_[page + in_page];
    changed_ = changed_ || byte != data[i];
    byte = data[i];
    in_page = (in_page + 1) & (kPageSize - 1);
  }
  statistics_.bytes_written += size;
}

void SaveMemory::page_program(std::uint32_t address, const std::uint8_t* data, std::size_t size) {
  const std::size_t page = (address % kSize) & ~(kPageSize - 1);
  std::size_t in_page = address & (kPageSize - 1);
  for (std::size_t i = 0; i < size; ++i) {
    std::uint8_t& byte = bytes_[page + in_page];
    const std::uint8_t result = byte & data[i];
    changed_ = changed_ || byte != result;
    byte = result;
    in_page = (in_page + 1) & (kPageSize - 1);
  }
  statistics_.bytes_written += size;
}

void SaveMemory::erase_sector(std::uint32_t address) {
  const std::size_t sector = (address % kSize) & ~(kSectorSize - 1);
  for (std::size_t i = 0; i < kSectorSize; ++i) {
    changed_ = changed_ || bytes_[sector + i] != 0xFF;
    bytes_[sector + i] = 0xFF;
  }
  ++statistics_.sectors_erased;
}

Status SaveMemory::load(ByteSpan data) {
  if (data.size() != kSize)
    return Error{ErrorCode::InvalidArgument, "a save for this game is exactly 262144 bytes"};
  std::copy(data.begin(), data.end(), bytes_.begin());
  changed_ = false;
  return {};
}

}  // namespace bd::nds
