#include "nds/dma.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

constexpr u32 kAddressMask = 0x0FFFFFFF;
constexpr u32 kCountMask = 0x001FFFFF;  // A count of zero means the maximum, 0x200000.
constexpr unsigned kDestinationShift = 21;  // 0 up, 1 down, 2 fixed, 3 up (and reloaded on repeat).
constexpr unsigned kSourceShift = 23;       // 0 up, 1 down, 2 fixed.
constexpr u32 kWords = 1u << 26;
constexpr u32 kRepeat = 1u << 25;           // With an event: stay enabled and go again at the next.
constexpr unsigned kStartShift = 27;        // 0 = at once; the rest wait for an event.
constexpr u32 kInterrupt = 1u << 30;
constexpr u32 kEnable = 1u << 31;

}  // namespace

Dma::Dma(runtime::Bus& bus, Interrupts& interrupts) : bus_(bus), interrupts_(interrupts) {}

void Dma::attach(Arm9Io& io) {
  for (int n = 0; n < kChannels; ++n) {
    const u32 base = 0x040000B0 + 12 * static_cast<u32>(n);
    io.add(base, 4, [this, n] { return channels_[n].source; },
           [this, n](u32 v, u32 m) { channels_[n].source = ((channels_[n].source & ~m) | (v & m)) & kAddressMask; });
    io.add(base + 4, 4, [this, n] { return channels_[n].destination; },
           [this, n](u32 v, u32 m) {
             channels_[n].destination = ((channels_[n].destination & ~m) | (v & m)) & kAddressMask;
           });
    io.add(base + 8, 4, [this, n] { return channels_[n].control; },
           [this, n](u32 v, u32 m) { write_control(n, v, m); });
    io.add(0x040000E0 + 4 * static_cast<u32>(n), 4, [this, n] { return fill_[n]; },
           [this, n](u32 v, u32 m) { fill_[n] = (fill_[n] & ~m) | (v & m); });
  }
}

void Dma::write_control(int channel, u32 value, u32 mask) {
  Channel& c = channels_[channel];
  const bool was_enabled = (c.control & kEnable) != 0;
  c.control = (c.control & ~mask) | (value & mask);
  // A transfer starts when the enable bit goes from 0 to 1.
  if (was_enabled || (c.control & kEnable) == 0) return;
  const bool words = (c.control & kWords) != 0;
  c.at_source = c.source & ~(words ? 3u : 1u);
  c.at_destination = c.destination & ~(words ? 3u : 1u);
  const unsigned start = (c.control >> kStartShift) & 7u;
  if (start != kStartAtOnce) {
    ++statistics_.waiting_for_event[start];
    if (on_waiting) on_waiting(start);
    return;
  }
  run(channel);
}

bool Dma::trigger(unsigned mode) {
  bool moved = false;
  for (int n = 0; n < kChannels; ++n) {
    const Channel& c = channels_[n];
    if ((c.control & kEnable) != 0 && ((c.control >> kStartShift) & 7u) == mode) {
      run(n);
      moved = true;
    }
  }
  return moved;
}

void Dma::run(int channel) {
  Channel& c = channels_[channel];
  u32 count = c.control & kCountMask;
  if (count == 0) count = kCountMask + 1;
  const bool words = (c.control & kWords) != 0;
  const u32 size = words ? 4 : 2;
  const unsigned source_mode = (c.control >> kSourceShift) & 3u;
  const unsigned destination_mode = (c.control >> kDestinationShift) & 3u;
  // Unsigned arithmetic: "down" is adding the two's complement.
  const u32 source_step = source_mode == 0 ? size : source_mode == 1 ? 0u - size : 0;
  const u32 destination_step = destination_mode == 1 ? 0u - size : destination_mode == 2 ? 0 : size;
  for (u32 i = 0; i < count; ++i) {
    if (words) {
      bus_.write32(c.at_destination, bus_.read32(c.at_source));
    } else {
      bus_.write16(c.at_destination, bus_.read16(c.at_source));
    }
    c.at_source += source_step;
    c.at_destination += destination_step;
  }
  ++statistics_.transfers;
  statistics_.units += count;
  const bool waits = ((c.control >> kStartShift) & 7u) != kStartAtOnce;
  if (waits && (c.control & kRepeat) != 0) {
    // Stays enabled for the next event. Mode 3 starts each round at the
    // destination again.
    if (destination_mode == 3) c.at_destination = c.destination & ~(size - 1);
  } else {
    c.control &= ~kEnable;
  }
  if (c.control & kInterrupt) interrupts_.request(8 + static_cast<unsigned>(channel));
}

}  // namespace bd::nds
