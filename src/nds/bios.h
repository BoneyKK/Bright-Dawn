// The BIOS calls of the DS main processor, done natively.
//
// A DS program reaches the console's BIOS through software interrupts. The
// BIOS itself is Nintendo's and is not used here; these are implementations
// of what the calls are documented to do (GBATEK). A call that is not
// implemented, or that is made with arguments whose result on a real console
// is not known, is reported and not guessed at.
//
// Implemented: WaitByLoop (0x03), Div (0x09), CpuSet (0x0B), CpuFastSet
// (0x0C), Sqrt (0x0D), GetCRC16 (0x0E), IsDebugger (0x0F), LZ77UnCompWram
// (0x11) and RLUnCompWram (0x14).
#pragma once

#include <cstdint>
#include <string>

#include "machine.h"

namespace bd::nds {

class Bios {
 public:
  explicit Bios(runtime::Bus& bus) : bus_(bus) {}

  struct Result {
    bool done = false;
    std::string problem;  // When not done: why.
  };

  // Performs call `number` on the registers in `cpu`. Registers that are not
  // a documented result keep their values; on a console r0-r3 and r12 may
  // come back changed, which callers cannot rely on either way.
  Result call(ds::Cpu& cpu, std::uint32_t number);

  // The name of a call as documentation gives it, or nullptr.
  static const char* name(std::uint32_t number);

 private:
  Result cpu_set(ds::Cpu& cpu);
  Result cpu_fast_set(ds::Cpu& cpu);
  Result divide(ds::Cpu& cpu);
  Result crc16(ds::Cpu& cpu);
  Result lz77(ds::Cpu& cpu);
  Result run_length(ds::Cpu& cpu);

  runtime::Bus& bus_;
};

}  // namespace bd::nds
