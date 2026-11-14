#include "nds/interrupts.h"

namespace bd::nds {

void Interrupts::attach(Arm9Io& io) {
  io.add(0x04000208, 4, [this] { return master_ ? 1u : 0u; },
         [this](std::uint32_t value, std::uint32_t mask) {
           if (mask & 1u) master_ = (value & 1u) != 0;
           changed();
         });
  io.add(0x04000210, 4, [this] { return enabled_; },
         [this](std::uint32_t value, std::uint32_t mask) {
           enabled_ = (enabled_ & ~mask) | (value & mask);
           changed();
         });
  io.add(0x04000214, 4, [this] { return requested_; },
         [this](std::uint32_t value, std::uint32_t mask) {
           requested_ &= ~(value & mask);
           changed();
         });
}

}  // namespace bd::nds
