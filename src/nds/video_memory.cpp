#include "nds/video_memory.h"

#include "nds/arm9_io.h"

namespace bd::nds {

namespace {

// Each bank's first page when it is plain memory at 0x06800000.
constexpr unsigned kPlainPage[VideoMemory::kBanks] = {0, 8, 16, 24, 32, 36, 37, 38, 40};

}  // namespace

VideoMemory::VideoMemory() {
  for (unsigned bank = 0; bank < kBanks; ++bank) banks_[bank].assign(kBankSizes[bank], 0);
}

void VideoMemory::attach(Arm9Io& io) {
  for (unsigned bank = 0; bank < kBanks; ++bank) {
    io.add(kControlAddresses[bank], 1, nullptr, [this, bank](std::uint32_t value, std::uint32_t mask) {
      if ((mask & 0xFF) != 0) set_control(static_cast<Bank>(bank), static_cast<std::uint8_t>(value));
    });
  }
}

void VideoMemory::set_control(Bank bank, std::uint8_t value) {
  value &= kControlBits[bank];
  if (value == control_[bank]) return;
  control_[bank] = value;
  place_banks();
}

void VideoMemory::place_banks() {
  main_backgrounds_.fill(Page{});
  sub_backgrounds_.fill(Page{});
  main_sprites_.fill(Page{});
  sub_sprites_.fill(Page{});
  plain_.fill(Page{});
  textures_.fill(Page{});
  texture_palettes_.fill(Page{});
  for (auto& screen : extended_palettes_) screen.fill(Page{});

  for (unsigned bank = 0; bank < kBanks; ++bank) {
    if ((control_[bank] & kEnable) == 0) continue;
    const unsigned use = control_[bank] & 7u;
    const unsigned offset = (control_[bank] >> 3) & 3u;
    std::uint8_t* memory = banks_[bank].data();
    const unsigned pages = static_cast<unsigned>(kBankSizes[bank] / kPageSize);
    // Puts the whole bank at `first` of `range`, a page after a page.
    const auto place = [&](auto& range, unsigned first) {
      for (unsigned i = 0; i < pages; ++i) range[first + i].add(memory + i * kPageSize);
    };

    if (use == 0) {
      place(plain_, kPlainPage[bank]);
      continue;
    }
    switch (bank) {
      case kA:
      case kB:
        if (use == 1) place(main_backgrounds_, offset * 8);
        if (use == 2) place(main_sprites_, (offset & 1) * 8);
        if (use == 3) place(textures_, offset * 8);
        break;
      case kC:
        if (use == 1) place(main_backgrounds_, offset * 8);
        if (use == 3) place(textures_, offset * 8);
        if (use == 4) place(sub_backgrounds_, 0);
        break;  // 2: the second processor's.
      case kD:
        if (use == 1) place(main_backgrounds_, offset * 8);
        if (use == 3) place(textures_, offset * 8);
        if (use == 4) place(sub_sprites_, 0);
        break;  // 2: the second processor's.
      case kE:
        if (use == 1) place(main_backgrounds_, 0);
        if (use == 2) place(main_sprites_, 0);
        if (use == 3) place(texture_palettes_, 0);
        if (use == 4) {
          // The first half of the bank: the four background slots.
          for (unsigned slot = 0; slot < 4; ++slot) extended_palettes_[0][slot].add(memory + slot * kExtendedPaletteSize);
        }
        break;
      case kF:
      case kG: {
        // One page, at 0, 1, 4 or 5 by the offset, and again two pages on.
        const unsigned first = ((offset & 2) << 1) + (offset & 1);
        if (use == 1) {
          main_backgrounds_[first].add(memory);
          main_backgrounds_[first + 2].add(memory);
        }
        if (use == 2) {
          main_sprites_[first].add(memory);
          main_sprites_[first + 2].add(memory);
        }
        if (use == 3) texture_palettes_[first].add(memory);
        if (use == 4) {
          // Two background slots: 0 and 1, or 2 and 3.
          for (unsigned slot = 0; slot < 2; ++slot)
            extended_palettes_[0][(offset & 1) * 2 + slot].add(memory + slot * kExtendedPaletteSize);
        }
        if (use == 5) extended_palettes_[0][4].add(memory);
        break;
      }
      case kH:
        if (use == 1) {
          // Two pages, and again four pages on.
          place(sub_backgrounds_, 0);
          place(sub_backgrounds_, 4);
        }
        if (use == 2) {
          for (unsigned slot = 0; slot < 4; ++slot) extended_palettes_[1][slot].add(memory + slot * kExtendedPaletteSize);
        }
        break;
      case kI:
        if (use == 1) {
          // One page, at pages 2, 3, 6 and 7.
          for (unsigned at : {2u, 3u, 6u, 7u}) sub_backgrounds_[at].add(memory);
        }
        if (use == 2) {
          // One page, repeated throughout.
          for (Page& page : sub_sprites_) page.add(memory);
        }
        if (use == 3) extended_palettes_[1][4].add(memory);
        break;
      default:
        break;
    }
  }
}

VideoMemory::Page& VideoMemory::page_at(std::uint32_t address) {
  switch (address & 0x00E00000) {
    case 0x00000000: return main_backgrounds_[(address & 0x7FFFF) / kPageSize];
    case 0x00200000: return sub_backgrounds_[(address & 0x1FFFF) / kPageSize];
    case 0x00400000: return main_sprites_[(address & 0x3FFFF) / kPageSize];
    case 0x00600000: return sub_sprites_[(address & 0x1FFFF) / kPageSize];
    default: return plain_[(address & 0xFFFFF) / kPageSize];
  }
}

std::uint32_t VideoMemory::read(std::uint32_t address, unsigned size) {
  const Page& page = page_at(address);
  if (page.count == 0) {
    ++statistics_.reads_with_no_bank;
    return 0;
  }
  const std::uint32_t at = address & (kPageSize - 1);
  std::uint32_t value = 0;
  for (unsigned m = 0; m < page.count; ++m) {
    for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(page.memory[m][at + i]) << (8 * i);
  }
  return value;
}

std::uint32_t VideoMemory::peek(std::uint32_t address, unsigned size) const {
  const Page& page = page_at(address);
  const std::uint32_t at = address & (kPageSize - 1);
  std::uint32_t value = 0;
  for (unsigned m = 0; m < page.count; ++m) {
    for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(page.memory[m][at + i]) << (8 * i);
  }
  return value;
}

void VideoMemory::write(std::uint32_t address, unsigned size, std::uint32_t value) {
  if (page_at(address).count == 0) {
    ++statistics_.writes_with_no_bank;
    return;
  }
  poke(address, size, value);
}

void VideoMemory::poke(std::uint32_t address, unsigned size, std::uint32_t value) {
  Page& page = page_at(address);
  const std::uint32_t at = address & (kPageSize - 1);
  for (unsigned m = 0; m < page.count; ++m) {
    for (unsigned i = 0; i < size; ++i) page.memory[m][at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

}  // namespace bd::nds
