// The game library's side of game_library.h: compiled into the library with
// the translated code, never into the program. What ds_cpu.h declares as
// provided by the platform layer is defined here, each a call through the
// functions the program hands over.
//
// Compiled with BD_GAME_BUILT_FOR defined as the Bright Dawn version the
// library is for, as a string.
#include <cstddef>

#include "game_library.h"

#ifndef BD_GAME_BUILT_FOR
#error "BD_GAME_BUILT_FOR must name the Bright Dawn version the library is built for"
#endif

#if defined(_WIN32)
#define BD_EXPORT __declspec(dllexport)
#else
#define BD_EXPORT __attribute__((visibility("default")))
#endif

namespace {

const BdHostFunctions* g_host = nullptr;

const BdGameLibrary kLibrary = {
    BD_GAME_LIBRARY_INTERFACE,
    static_cast<std::uint32_t>(sizeof(ds::Cpu)),
    static_cast<std::uint32_t>(offsetof(ds::Cpu, depth)),
    static_cast<std::uint32_t>(sizeof(TranslatedEntry)),
    BD_GAME_BUILT_FOR,
    translated_table,
};

}  // namespace

namespace ds {

u8 Cpu::rd8(u32 a) { return g_host->rd8(this, a); }
u16 Cpu::rd16(u32 a) { return g_host->rd16(this, a); }
u32 Cpu::rd32(u32 a) { return g_host->rd32(this, a); }
void Cpu::wr8(u32 a, u8 v) { g_host->wr8(this, a, v); }
void Cpu::wr16(u32 a, u16 v) { g_host->wr16(this, a, v); }
void Cpu::wr32(u32 a, u32 v) { g_host->wr32(this, a, v); }

bool leave_slow(Cpu& c, u32 ret) { return g_host->leave_slow(&c, ret); }
void call(Cpu& c, u32 address) { g_host->call(&c, address); }
void jump(Cpu& c, u32 address) { g_host->jump(&c, address); }
bool swi(Cpu& c, u32 number, u32 return_address) { return g_host->swi(&c, number, return_address); }
bool interrupt(Cpu& c, u32 next) { return g_host->interrupt(&c, next); }
void cp_write(Cpu& c, int cp, int opc1, int crn, int crm, int opc2, u32 value) {
  g_host->cp_write(&c, cp, opc1, crn, crm, opc2, value);
}
u32 cp_read(Cpu& c, int cp, int opc1, int crn, int crm, int opc2) {
  return g_host->cp_read(&c, cp, opc1, crn, crm, opc2);
}
void unhandled(Cpu& c, u32 address, const char* what) { g_host->unhandled(&c, address, what); }

}  // namespace ds

extern "C" BD_EXPORT const BdGameLibrary* bd_game_library(const BdHostFunctions* host) {
  if (host == nullptr || host->interface_version != BD_GAME_LIBRARY_INTERFACE || host->size < sizeof(BdHostFunctions))
    return nullptr;
  g_host = host;
  return &kLibrary;
}
