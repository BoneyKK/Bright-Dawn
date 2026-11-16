// The game library: the game's code translated to C++ from the player's ROM
// and compiled, on their computer, into a library of its own (a .dll on
// Windows, a .so elsewhere) that the program loads when it plays.
//
// The library may be compiled by another compiler than the program (the game
// builder uses Zig's Clang; the program may be built with MSVC), so what
// passes between them is plain C: this header's two structures, the
// processor model in ds_cpu.h, and functions that take pointers and whole
// numbers. The library exports one function, bd_game_library. The program
// calls it with the functions translated code needs of it (memory, calls and
// jumps it cannot bind itself, BIOS calls, the system coprocessor); it
// answers with its table of translated functions, and with what it was built
// for, which the program checks before using any of it.
//
// game_library_glue.cpp is the library's side: it defines what ds_cpu.h
// declares as provided by the platform layer, each a call through the
// program's functions.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ds_cpu.h"
#include "translated_table.h"

// Changes whenever what passes between the program and the library changes:
// this header, ds_cpu.h, translated_table.h. A library of another interface
// is not used.
#define BD_GAME_LIBRARY_INTERFACE 2

// The name of the function the library exports.
#define BD_GAME_LIBRARY_ENTRY "bd_game_library"

extern "C" {

// What the program provides, in the order ds_cpu.h declares it.
struct BdHostFunctions {
  std::uint32_t interface_version;  // BD_GAME_LIBRARY_INTERFACE
  std::uint32_t size;               // sizeof(BdHostFunctions)
  ds::u8 (*rd8)(ds::Cpu* cpu, ds::u32 address);
  ds::u16 (*rd16)(ds::Cpu* cpu, ds::u32 address);
  ds::u32 (*rd32)(ds::Cpu* cpu, ds::u32 address);
  void (*wr8)(ds::Cpu* cpu, ds::u32 address, ds::u8 value);
  void (*wr16)(ds::Cpu* cpu, ds::u32 address, ds::u16 value);
  void (*wr32)(ds::Cpu* cpu, ds::u32 address, ds::u32 value);
  bool (*leave_slow)(ds::Cpu* cpu, ds::u32 return_address);
  void (*call)(ds::Cpu* cpu, ds::u32 address);
  void (*jump)(ds::Cpu* cpu, ds::u32 address);
  bool (*swi)(ds::Cpu* cpu, ds::u32 number, ds::u32 return_address);
  void (*cp_write)(ds::Cpu* cpu, int cp, int opc1, int crn, int crm, int opc2, ds::u32 value);
  ds::u32 (*cp_read)(ds::Cpu* cpu, int cp, int opc1, int crn, int crm, int opc2);
  void (*unhandled)(ds::Cpu* cpu, ds::u32 address, const char* what);
  bool (*interrupt)(ds::Cpu* cpu, ds::u32 next);
};

// What the library answers. The sizes and the offset let the program check
// that both were compiled with the same layout of the shared structures.
struct BdGameLibrary {
  std::uint32_t interface_version;  // BD_GAME_LIBRARY_INTERFACE as compiled into the library
  std::uint32_t cpu_size;           // sizeof(ds::Cpu)
  std::uint32_t cpu_depth_offset;   // offsetof(ds::Cpu, depth)
  std::uint32_t entry_size;         // sizeof(TranslatedEntry)
  const char* built_for;            // The Bright Dawn version it was built for.
  const TranslatedEntry* table;     // Ends with an entry whose function is null.
};

// Returns null when `host` is not of the library's interface.
typedef const BdGameLibrary* (*BdGameLibraryEntry)(const BdHostFunctions* host);

}  // extern "C"
