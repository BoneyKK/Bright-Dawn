// Adapters that let the runtime's Machine work on the test guest memory.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "guest_runner.h"
#include "machine.h"

namespace guest {

class Bus : public bd::runtime::Bus {
 public:
  explicit Bus(Memory& memory) : memory_(memory) {}
  std::uint8_t read8(std::uint32_t a) override { return *memory_.at(a, 1); }
  std::uint16_t read16(std::uint32_t a) override { std::uint16_t v; std::memcpy(&v, memory_.at(a, 2), 2); return v; }
  std::uint32_t read32(std::uint32_t a) override { std::uint32_t v; std::memcpy(&v, memory_.at(a, 4), 4); return v; }
  void write8(std::uint32_t a, std::uint8_t v) override { *memory_.at(a, 1) = v; }
  void write16(std::uint32_t a, std::uint16_t v) override { std::memcpy(memory_.at(a, 2), &v, 2); }
  void write32(std::uint32_t a, std::uint32_t v) override { std::memcpy(memory_.at(a, 4), &v, 4); }

 private:
  Memory& memory_;
};

// The test programs use one software interrupt and nothing else outside the
// processor; any other request is recorded as a problem.
class System : public bd::runtime::System {
 public:
  void software_interrupt(ds::Cpu& cpu, std::uint32_t number) override {
    if (number == kTestSoftwareInterrupt) {
      cpu.r[0] = test_software_interrupt(cpu.r[0], cpu.r[1]);
    } else {
      note("software interrupt %u", number);
    }
  }
  std::uint32_t coprocessor_read(ds::Cpu&, int, int, int, int, int) override { note("coprocessor read%.0u", 0); return 0; }
  void coprocessor_write(ds::Cpu&, int, int, int, int, int, std::uint32_t) override { note("coprocessor write%.0u", 0); }
  void unhandled(ds::Cpu&, std::uint32_t address, const char* what) override {
    char text[160];
    std::snprintf(text, sizeof text, "unhandled at %08x: %s", static_cast<unsigned>(address), what);
    if (problem.empty()) problem = text;
  }

  // The first thing that went wrong since this was last cleared.
  std::string problem;

 private:
  void note(const char* format, std::uint32_t value) {
    char text[64];
    std::snprintf(text, sizeof text, format, static_cast<unsigned>(value));
    if (problem.empty()) problem = text;
  }
};

}  // namespace guest
