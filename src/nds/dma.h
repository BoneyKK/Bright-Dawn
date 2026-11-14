// The four DMA channels of the main processor: hardware that copies or fills
// memory on the program's behalf.
//
// Transfers that start at once are performed, and so are transfers fed by
// the cartridge, a word at a time as the cartridge delivers. A transfer set
// to start on any other hardware event (vertical or horizontal blank, the
// geometry queue) stays pending and is counted, because nothing raises those
// events for DMA yet.
#pragma once

#include <cstdint>
#include <functional>

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

  // Start modes (bits 27-29 of a channel's control register).
  static constexpr unsigned kStartAtOnce = 0;
  static constexpr unsigned kStartCartridge = 5;

  // The event of start mode `mode` has happened. Every channel waiting for
  // it moves the number of units it is set to and, unless set to repeat, is
  // then finished. Returns whether any channel moved anything.
  bool trigger(unsigned mode);

  // Called when a channel has been enabled in a mode that waits for an
  // event, with the mode: the event may already be due.
  std::function<void(unsigned mode)> on_waiting;

  struct Statistics {
    std::uint64_t transfers = 0;       // Performed.
    std::uint64_t units = 0;           // 16- or 32-bit units moved.
    // Transfers the program started in a mode that waits for an event, by
    // start mode. Apart from the cartridge's, they never run.
    std::uint64_t waiting_for_event[8] = {};
  };
  const Statistics& statistics() const { return statistics_; }

 private:
  struct Channel {
    std::uint32_t source = 0, destination = 0, control = 0;
    // Where a transfer in progress has got to. Taken from the registers when
    // the channel is enabled.
    std::uint32_t at_source = 0, at_destination = 0;
  };
  void write_control(int channel, std::uint32_t value, std::uint32_t mask);
  // Moves the channel's count of units, then finishes it or, for a repeating
  // transfer that waits for events, leaves it enabled for the next.
  void run(int channel);

  runtime::Bus& bus_;
  Interrupts& interrupts_;
  Channel channels_[kChannels];
  std::uint32_t fill_[kChannels] = {};
  Statistics statistics_;
};

}  // namespace bd::nds
