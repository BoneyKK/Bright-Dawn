// The four DMA channels of the main processor: hardware that copies or fills
// memory on the program's behalf.
//
// Only transfers that start at once are performed. A transfer set to start on
// a hardware event (vertical or horizontal blank, the cartridge, the
// geometry queue) stays pending and is counted, because none of those events
// exists yet.
#pragma once

#include <cstdint>

#include "machine.h"
#include "nds/arm9_io.h"
#include "nds/interrupts.h"

namespace bd::nds {

class Dma {
 public:
  static constexpr int kChannels = 4;

  // Transfers read and write through `bus`.
  Dma(runtime::Bus& bus, Interrupts& interrupts);

  // Registers per channel n: source (0x040000B0 + 12n), destination (+4) and
  // control (+8); and the four fill-data registers at 0x040000E0, which are
  // plain storage a program uses as the source of a fill.
  void attach(Arm9Io& io);

  struct Statistics {
    std::uint64_t transfers = 0;       // Performed.
    std::uint64_t units = 0;           // 16- or 32-bit units moved.
    // Transfers the program started in a mode that waits for an event, by
    // start mode (bits 27-29 of the control register). They never run.
    std::uint64_t waiting_for_event[8] = {};
  };
  const Statistics& statistics() const { return statistics_; }

 private:
  struct Channel {
    std::uint32_t source = 0, destination = 0, control = 0;
  };
  void write_control(int channel, std::uint32_t value, std::uint32_t mask);
  void run(int channel);

  runtime::Bus& bus_;
  Interrupts& interrupts_;
  Channel channels_[kChannels];
  std::uint32_t fill_[kChannels] = {};
  Statistics statistics_;
};

}  // namespace bd::nds
