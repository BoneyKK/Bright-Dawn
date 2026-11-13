#include "nds/system_control.h"

#include <algorithm>

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

constexpr u32 key(int crn, int crm, int opc2) {
  return (static_cast<u32>(crn) << 8) | (static_cast<u32>(crm) << 4) | static_cast<u32>(opc2);
}

// The bits of the control register that can be changed. The others are fixed:
// bits 3-6 read as one, the rest as zero.
constexpr u32 kControlWritable = 0x000FF085;

// A region size field: 512 bytes shifted left by the field. The fast
// memories cannot be mapped smaller than 4 KiB.
std::uint64_t region_size(u32 reg) {
  return std::max<std::uint64_t>(0x1000, std::uint64_t{0x200} << ((reg >> 1) & 0x1F));
}

}  // namespace

SystemControl::SystemControl(Arm9Bus& bus) : bus_(bus) { apply(); }

void SystemControl::set_firmware_state() {
  write(1, 0, 0, 0x0005707D);
  write(9, 1, 0, 0x0300000A);  // Data memory: 16 KiB at 0x03000000.
  write(9, 1, 1, 0x00000020);  // Instruction memory: the first 32 MiB.
}

void SystemControl::apply() {
  // Control bits 16 and 18 enable the data and instruction memory; bits 17
  // and 19 put one in "load mode", where it takes writes but not reads.
  const bool dtcm_on = (control_ & (1u << 16)) != 0, dtcm_load = (control_ & (1u << 17)) != 0;
  const bool itcm_on = (control_ & (1u << 18)) != 0, itcm_load = (control_ & (1u << 19)) != 0;
  bus_.map_dtcm(dtcm_on && !dtcm_load, dtcm_on, dtcm_ & 0xFFFFF000u, region_size(dtcm_));
  bus_.map_itcm(itcm_on && !itcm_load, itcm_on, region_size(itcm_));
}

u32 SystemControl::read(int crn, int crm, int opc2) {
  switch (key(crn, crm, opc2)) {
    case key(0, 0, 0): return 0x41059461;  // Processor: ARM946E-S.
    case key(0, 0, 1): return 0x0F0D2112;  // Cache sizes.
    case key(0, 0, 2): return 0x00140180;  // Fast memory sizes.
    case key(1, 0, 0): return control_;
    case key(2, 0, 0): return cache_config_[0];
    case key(2, 0, 1): return cache_config_[1];
    case key(3, 0, 0): return write_buffer_;
    case key(5, 0, 0): return access_[0];
    case key(5, 0, 1): return access_[1];
    case key(5, 0, 2): return access_[2];
    case key(5, 0, 3): return access_[3];
    case key(9, 1, 0): return dtcm_;
    case key(9, 1, 1): return itcm_;
    case key(13, 0, 1): case key(13, 1, 1): return process_id_;
    default: break;
  }
  if (crn == 6 && crm < 8 && (opc2 == 0 || opc2 == 1)) return region_[crm];
  ++unknown_reads_[key(crn, crm, opc2)];
  return 0;
}

void SystemControl::write(int crn, int crm, int opc2, u32 value) {
  switch (key(crn, crm, opc2)) {
    case key(1, 0, 0):
      control_ = (control_ & ~kControlWritable) | (value & kControlWritable);
      apply();
      return;
    case key(2, 0, 0): cache_config_[0] = value; return;
    case key(2, 0, 1): cache_config_[1] = value; return;
    case key(3, 0, 0): write_buffer_ = value; return;
    case key(5, 0, 0): access_[0] = value; return;
    case key(5, 0, 1): access_[1] = value; return;
    case key(5, 0, 2): access_[2] = value; return;
    case key(5, 0, 3): access_[3] = value; return;
    case key(9, 1, 0): dtcm_ = value; apply(); return;
    case key(9, 1, 1): itcm_ = value; apply(); return;
    case key(13, 0, 1): case key(13, 1, 1): process_id_ = value; return;
    case key(7, 0, 4): case key(7, 8, 2): wait_requested_ = true; return;
    default: break;
  }
  if (crn == 6 && crm < 8 && (opc2 == 0 || opc2 == 1)) {
    region_[crm] = value;
    return;
  }
  // Cache maintenance (flush, clean, prefetch): there are no caches here, so
  // there is nothing to do. Cache lockdown likewise.
  if (crn == 7 || (crn == 9 && crm == 0)) return;
  ++unknown_writes_[key(crn, crm, opc2)];
}

}  // namespace bd::nds
