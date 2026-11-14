// The interrupt controller of the main processor: which sources have
// requested an interrupt, which are enabled, and the master switch.
//
// This only keeps the three registers. Making the processor take the
// interrupt is the console's business.
#pragma once

#include <cstdint>
#include <functional>

#include "nds/arm9_io.h"

namespace bd::nds {

class Interrupts {
 public:
  // Bit numbers in the enable and request registers.
  enum Source : unsigned {
    kVBlank = 0,
    kHBlank = 1,
    kVCount = 2,
    kTimer0 = 3,
    kIpcSync = 16,
    kIpcSendEmpty = 17,
    kIpcReceive = 18,
    kCartridgeTransfer = 19,
    kGeometryFifo = 21,
  };

  // Registers: master enable (0x04000208), enable (0x04000210) and request
  // (0x04000214, where writing a one clears the bit).
  void attach(Arm9Io& io);

  void request(unsigned source) {
    requested_ |= 1u << source;
    changed();
  }

  // Called whenever pending() may have changed.
  std::function<void()> on_change;

  // Whether an enabled source is waiting and the master switch is on. The
  // processor's own interrupt mask is not considered here.
  bool pending() const { return master_ && (enabled_ & requested_) != 0; }
  // The same without the master switch: what ends a wait for an interrupt.
  bool any_enabled_requested() const { return (enabled_ & requested_) != 0; }

  bool master() const { return master_; }
  std::uint32_t enabled() const { return enabled_; }
  std::uint32_t requested() const { return requested_; }

 private:
  void changed() {
    if (on_change) on_change();
  }
  bool master_ = false;
  std::uint32_t enabled_ = 0, requested_ = 0;
};

}  // namespace bd::nds
