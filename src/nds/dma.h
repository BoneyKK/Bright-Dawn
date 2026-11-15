// The four DMA channels of the main processor: hardware that copies or fills
// memory on the program's behalf.
//
// Transfers that start at once are performed; so are transfers fed by the
// cartridge, a word at a time as the cartridge delivers, transfers that
// start with the vertical blank or with the horizontal blank of each visible
// line, and transfers that feed the 3D engine's command queue while it is
// less than half full. A transfer set to start on any other hardware event
// (the start of the display, the second cartridge slot) stays pending and is
// counted, because nothing raises those events for DMA yet.
//
// A transfer to the 3D engine's queue moves all its words at once here. The
// hardware moves 112 at a time and waits for the queue to drain in between;
// with a queue that never fills while the engine runs, the outcome is the
// same.
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
  static constexpr unsigned kStartVBlank = 1;
  static constexpr unsigned kStartHBlank = 2;  // Of visible lines only.
  static constexpr unsigned kStartCartridge = 5;
  static constexpr unsigned kStartGeometry = 7;

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
    // start mode. Those of modes other than the four above never run.
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
