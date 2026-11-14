#include "nds/bios.h"

#include <cmath>

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

constexpr u32 kCountMask = 0x001FFFFF;  // CpuSet, CpuFastSet: units to move.
constexpr u32 kFill = 1u << 24;         // Fill from one source unit instead of copying.
constexpr u32 kWords = 1u << 26;        // CpuSet: 32-bit units instead of 16-bit.

Bios::Result done() {
  Bios::Result r;
  r.done = true;
  return r;
}

Bios::Result problem(std::string what) {
  Bios::Result r;
  r.problem = std::move(what);
  return r;
}

// The integer square root: the largest r with r * r <= value.
u32 integer_sqrt(u32 value) {
  u32 root = 0;
  for (u32 bit = 1u << 15; bit != 0; bit >>= 1) {
    const u32 trial = root | bit;
    if (static_cast<std::uint64_t>(trial) * trial <= value) root = trial;
  }
  return root;
}

}  // namespace

const char* Bios::name(u32 number) {
  switch (number) {
    case 0x00: return "SoftReset";
    case 0x03: return "WaitByLoop";
    case 0x04: return "IntrWait";
    case 0x05: return "VBlankIntrWait";
    case 0x06: return "Halt";
    case 0x09: return "Div";
    case 0x0B: return "CpuSet";
    case 0x0C: return "CpuFastSet";
    case 0x0D: return "Sqrt";
    case 0x0E: return "GetCRC16";
    case 0x0F: return "IsDebugger";
    case 0x10: return "BitUnPack";
    case 0x11: return "LZ77UnCompWram";
    case 0x12: return "LZ77UnCompVram";
    case 0x13: return "HuffUnComp";
    case 0x14: return "RLUnCompWram";
    case 0x15: return "RLUnCompVram";
    case 0x16: return "Diff8bitUnFilterWram";
    case 0x18: return "Diff16bitUnFilter";
    case 0x1F: return "CustomPost";
    default: return nullptr;
  }
}

Bios::Result Bios::call(ds::Cpu& cpu, u32 number) {
  switch (number) {
    case 0x03:
      // Counts r0 down to zero to pass time. Time is not modelled: it is over
      // at once.
      cpu.r[0] = 0;
      return done();
    case 0x09: return divide(cpu);
    case 0x0B: return cpu_set(cpu);
    case 0x0C: return cpu_fast_set(cpu);
    case 0x0D:
      cpu.r[0] = integer_sqrt(cpu.r[0]);
      return done();
    case 0x0E: return crc16(cpu);
    case 0x0F:
      cpu.r[0] = 0;  // Not a development unit.
      return done();
    case 0x11: return lz77(cpu);
    case 0x14: return run_length(cpu);
    default: break;
  }
  const char* known = name(number);
  return problem(std::string("BIOS call ") + (known ? known : "with an unknown number") + " is not implemented");
}

// r0 = numerator, r1 = denominator, both signed.
// Returns r0 = quotient, r1 = remainder, r3 = absolute value of the quotient.
Bios::Result Bios::divide(ds::Cpu& cpu) {
  const std::int64_t numerator = static_cast<std::int32_t>(cpu.r[0]);
  const std::int64_t denominator = static_cast<std::int32_t>(cpu.r[1]);
  if (denominator == 0) return problem("BIOS call Div with a denominator of zero: the result on a console is not known here");
  const std::int64_t quotient = numerator / denominator;  // 64-bit: -2^31 / -1 does not overflow.
  const std::int64_t remainder = numerator % denominator;
  cpu.r[0] = static_cast<u32>(quotient);
  cpu.r[1] = static_cast<u32>(remainder);
  cpu.r[3] = static_cast<u32>(quotient < 0 ? -quotient : quotient);
  return done();
}

// r0 = source, r1 = destination, r2 = number of units and mode.
Bios::Result Bios::cpu_set(ds::Cpu& cpu) {
  const u32 count = cpu.r[2] & kCountMask;
  const bool fill = (cpu.r[2] & kFill) != 0;
  if (cpu.r[2] & kWords) {
    const u32 source = cpu.r[0] & ~3u, destination = cpu.r[1] & ~3u;
    if (fill) {
      const u32 value = bus_.read32(source);  // Read once, then stored repeatedly.
      for (u32 i = 0; i < count; ++i) bus_.write32(destination + 4 * i, value);
    } else {
      for (u32 i = 0; i < count; ++i) bus_.write32(destination + 4 * i, bus_.read32(source + 4 * i));
    }
  } else {
    const u32 source = cpu.r[0] & ~1u, destination = cpu.r[1] & ~1u;
    if (fill) {
      const std::uint16_t value = bus_.read16(source);
      for (u32 i = 0; i < count; ++i) bus_.write16(destination + 2 * i, value);
    } else {
      for (u32 i = 0; i < count; ++i) bus_.write16(destination + 2 * i, bus_.read16(source + 2 * i));
    }
  }
  return done();
}

// As CpuSet, always in 32-bit units. (On the earlier console the count was
// rounded up to a multiple of eight; on the DS it is taken as given.)
Bios::Result Bios::cpu_fast_set(ds::Cpu& cpu) {
  const u32 count = cpu.r[2] & kCountMask;
  const u32 source = cpu.r[0] & ~3u, destination = cpu.r[1] & ~3u;
  if (cpu.r[2] & kFill) {
    const u32 value = bus_.read32(source);
    for (u32 i = 0; i < count; ++i) bus_.write32(destination + 4 * i, value);
  } else {
    for (u32 i = 0; i < count; ++i) bus_.write32(destination + 4 * i, bus_.read32(source + 4 * i));
  }
  return done();
}

// r0 = initial value, r1 = address, r2 = length in bytes. Returns the CRC-16
// (polynomial 0xA001, reflected) in r0.
Bios::Result Bios::crc16(ds::Cpu& cpu) {
  if (cpu.r[2] & 1u) return problem("BIOS call GetCRC16 with an odd length: the result on a console is not known here");
  u32 crc = cpu.r[0] & 0xFFFFu;
  for (u32 i = 0; i < cpu.r[2]; ++i) {
    crc ^= bus_.read8(cpu.r[1] + i);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xA001u : 0u);
  }
  cpu.r[0] = crc;
  return done();
}

// r0 = source (a header word, then the data), r1 = destination. The header's
// top 24 bits are the size of the result.
Bios::Result Bios::lz77(ds::Cpu& cpu) {
  const u32 size = bus_.read32(cpu.r[0] & ~3u) >> 8;
  u32 source = cpu.r[0] + 4;
  const u32 destination = cpu.r[1];
  u32 written = 0;
  while (written < size) {
    u32 flags = bus_.read8(source++);
    for (int i = 0; i < 8 && written < size; ++i, flags <<= 1) {
      if (flags & 0x80u) {
        // A run copied from earlier in the result.
        const u32 first = bus_.read8(source++), second = bus_.read8(source++);
        const u32 length = 3 + (first >> 4);
        const u32 distance = 1 + (((first & 15u) << 8) | second);
        for (u32 j = 0; j < length && written < size; ++j, ++written)
          bus_.write8(destination + written, bus_.read8(destination + written - distance));
      } else {
        bus_.write8(destination + written++, bus_.read8(source++));
      }
    }
  }
  return done();
}

// r0 = source, r1 = destination; header as for LZ77UnCompWram.
Bios::Result Bios::run_length(ds::Cpu& cpu) {
  const u32 size = bus_.read32(cpu.r[0] & ~3u) >> 8;
  u32 source = cpu.r[0] + 4;
  const u32 destination = cpu.r[1];
  u32 written = 0;
  while (written < size) {
    const u32 flag = bus_.read8(source++);
    if (flag & 0x80u) {
      const std::uint8_t value = bus_.read8(source++);
      for (u32 j = 0; j < (flag & 0x7Fu) + 3 && written < size; ++j) bus_.write8(destination + written++, value);
    } else {
      for (u32 j = 0; j < (flag & 0x7Fu) + 1 && written < size; ++j)
        bus_.write8(destination + written++, bus_.read8(source++));
    }
  }
  return done();
}

}  // namespace bd::nds
