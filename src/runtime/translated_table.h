// The table a translated program exports: every translated function by guest
// address.
#pragma once

#include "ds_cpu.h"

// A stretch of guest memory that a translation was made from: instructions,
// and constants the translator read from next to them.
struct TranslatedRun {
  ds::u32 address;
  ds::u32 length;
};

struct TranslatedEntry {
  // Guest address of the function; bit 0 is set for Thumb functions.
  ds::u32 address;
  void (*function)(ds::Cpu&);
  // -1 for code that is always in memory (the main program). Otherwise the
  // overlay the function belongs to. Several overlays can be translated for
  // the same address; the run-time uses the one whose code is in memory.
  int overlay;
  // What the translation was made from. Unused when overlay is -1.
  const TranslatedRun* runs;
  ds::u32 run_count;
};

// Sorted by address, and ended by an entry whose function is null.
extern const TranslatedEntry translated_table[];
