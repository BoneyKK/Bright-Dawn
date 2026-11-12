#pragma once

#include <cstdint>

#include "core/bytes.h"

namespace bd {

// CRC-16/MODBUS (reflected polynomial 0xA001, initial value 0xFFFF). This is
// the checksum the DS cartridge header uses for itself and for the logo.
std::uint16_t crc16(ByteSpan data);

// Standard CRC-32 (IEEE 802.3), as used by ROM dump databases.
std::uint32_t crc32(ByteSpan data);

}  // namespace bd
