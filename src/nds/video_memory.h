// The DS's video memory: nine banks, each of which the program places where
// it needs it.
//
// There are 656 KiB of video memory in nine banks, A to I, of different
// sizes. A control register per bank says whether the bank is in use and what
// for: as backgrounds or sprites of either screen, as textures or texture
// palettes for the 3D engine, as extra palettes, or as plain memory. Only
// some of those uses give the bank an address the main processor can reach.
// A game moves banks around all the time; this one loads textures into a
// bank while it is plain memory and then hands the bank to the 3D engine.
//
// Addresses, in pages of 16 KiB:
//   0x06000000  backgrounds of the main screen, 512 KiB, then repeating
//   0x06200000  backgrounds of the second screen, 128 KiB, then repeating
//   0x06400000  sprites of the main screen, 256 KiB, then repeating
//   0x06600000  sprites of the second screen, 128 KiB, then repeating
//   0x06800000  plain memory, every bank at its own fixed place, repeating
//               every 1 MiB
// Where no bank is, reads give zero and writes are lost; both are counted.
// Where several banks are at the same place, a read gives their contents
// ORed together and a write goes to all of them.
//
// Which setting puts which bank where is GBATEK's table, checked against
// what NooDS does, including where the small banks repeat inside the space
// they are given. They differ in one case, banks A and B as sprite memory
// with an offset above 1, which NooDS does not handle; GBATEK's reading
// (only the low bit of the offset counts) is used.
//
// Banks C and D can also be handed to the second processor. They are then
// out of the main processor's reach; nothing here uses them from that side.
//
// The 3D engine reads textures and texture palettes from the banks handed to
// it: textures from four slots of 128 KiB, which banks A to D can fill, and
// palettes from six slots of 16 KiB, of which bank E fills the first four
// and banks F and G one each. That is a memory of its own, not reachable by
// the main processor. Banks at the same slot read ORed together, as
// elsewhere here; that case is an assumption (NooDS takes the last bank).
//
// The 2D display hardware reads backgrounds and sprites through the same
// addresses as the main processor (peek() does that without counting), and
// its extended palettes from banks handed over for that: for each screen
// four slots of 8 KiB for backgrounds and one for sprites.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bd::nds {

class Arm9Io;

class VideoMemory {
 public:
  enum Bank : unsigned { kA, kB, kC, kD, kE, kF, kG, kH, kI, kBanks };
  static constexpr std::size_t kBankSizes[kBanks] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                                     0x4000,  0x4000,  0x8000,  0x4000};
  // The bits of each bank's control register that exist: the use (bits 0-2,
  // fewer for some banks), an offset (bits 3-4, where the use has one) and
  // the enable bit (7).
  static constexpr std::uint8_t kControlBits[kBanks] = {0x9B, 0x9B, 0x9F, 0x9F, 0x87, 0x9F, 0x9F, 0x83, 0x83};
  static constexpr std::uint8_t kEnable = 0x80;
  // Where each bank's control register is. (0x04000247 belongs to something
  // else.)
  static constexpr std::uint32_t kControlAddresses[kBanks] = {0x04000240, 0x04000241, 0x04000242,
                                                             0x04000243, 0x04000244, 0x04000245,
                                                             0x04000246, 0x04000248, 0x04000249};

  static constexpr std::uint32_t kPageSize = 0x4000;

  struct Statistics {
    std::uint64_t reads_with_no_bank = 0;
    std::uint64_t writes_with_no_bank = 0;
  };

  VideoMemory();

  // The control registers. They can be written only; they read as zero.
  void attach(Arm9Io& io);

  // `address` is anywhere in 0x06000000 - 0x06FFFFFF, aligned to `size` (1, 2
  // or 4).
  std::uint32_t read(std::uint32_t address, unsigned size);
  void write(std::uint32_t address, unsigned size, std::uint32_t value);

  std::uint8_t control(Bank bank) const { return control_[bank]; }
  void set_control(Bank bank, std::uint8_t value);
  // What the 3D engine reads: a byte of texture memory (addresses 0 to
  // 0x7FFFF) or of texture palette memory (0 to 0x17FFF), or -1 where no
  // bank is.
  static constexpr std::uint32_t kTextureSize = 0x80000, kTexturePaletteSize = 0x18000;
  int texture(std::uint32_t address) const { return seen(textures_[(address & (kTextureSize - 1)) / kPageSize], address); }
  int texture_palette(std::uint32_t address) const {
    return address < kTexturePaletteSize ? seen(texture_palettes_[address / kPageSize], address) : -1;
  }

  // What the 2D display hardware reads. peek() is read() without the
  // counting: zero where no bank is.
  std::uint32_t peek(std::uint32_t address, unsigned size) const;
  // What the display capture writes: write() without the counting.
  void poke(std::uint32_t address, unsigned size, std::uint32_t value);
  // A byte of an extended palette, or -1 where no bank is: `slot` 0 to 3 are
  // the backgrounds' and 4 is the sprites', each 8 KiB.
  static constexpr unsigned kExtendedPaletteSlots = 5;
  static constexpr std::uint32_t kExtendedPaletteSize = 0x2000;
  int extended_palette(bool second_screen, unsigned slot, std::uint32_t offset) const {
    const Page& page = extended_palettes_[second_screen ? 1 : 0][slot % kExtendedPaletteSlots];
    if (page.count == 0) return -1;
    int value = 0;
    for (unsigned m = 0; m < page.count; ++m) value |= page.memory[m][offset & (kExtendedPaletteSize - 1)];
    return value;
  }

  // A bank's own contents, wherever it is placed.
  const std::vector<std::uint8_t>& contents(Bank bank) const { return banks_[bank]; }

  const Statistics& statistics() const { return statistics_; }

 private:
  // What is at one page of an address range: the same page of up to seven
  // banks (all but H and I can be backgrounds of the main screen at once).
  struct Page {
    std::array<std::uint8_t*, 7> memory{};
    unsigned count = 0;
    void add(std::uint8_t* at) { memory[count++] = at; }
  };
  static constexpr unsigned kMainBackgroundPages = 32, kSubBackgroundPages = 8, kMainSpritePages = 16,
                            kSubSpritePages = 8, kPlainPages = 64, kTexturePages = 32, kTexturePalettePages = 6;
  static int seen(const Page& page, std::uint32_t address) {
    if (page.count == 0) return -1;
    int value = 0;
    for (unsigned m = 0; m < page.count; ++m) value |= page.memory[m][address & (kPageSize - 1)];
    return value;
  }

  void place_banks();
  Page& page_at(std::uint32_t address);
  const Page& page_at(std::uint32_t address) const { return const_cast<VideoMemory*>(this)->page_at(address); }

  std::array<std::vector<std::uint8_t>, kBanks> banks_;
  std::array<std::uint8_t, kBanks> control_{};
  std::array<Page, kMainBackgroundPages> main_backgrounds_{};
  std::array<Page, kSubBackgroundPages> sub_backgrounds_{};
  std::array<Page, kMainSpritePages> main_sprites_{};
  std::array<Page, kSubSpritePages> sub_sprites_{};
  std::array<Page, kPlainPages> plain_{};
  std::array<Page, kTexturePages> textures_{};
  std::array<Page, kTexturePalettePages> texture_palettes_{};
  // By screen and slot; each entry points at 8 KiB.
  std::array<std::array<Page, kExtendedPaletteSlots>, 2> extended_palettes_{};
  Statistics statistics_;
};

}  // namespace bd::nds
