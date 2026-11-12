// The table a translated program exports: every translated function by guest
// address. Bit 0 of the address is set for Thumb functions. The table is
// sorted by address and ends with an entry whose function is null.
#pragma once

#include "ds_cpu.h"

struct TranslatedEntry {
  ds::u32 address;
  void (*function)(ds::Cpu&);
};

extern const TranslatedEntry translated_table[];
