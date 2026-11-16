#include "game_library_host.h"

#include <cstddef>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bd::runtime {
namespace {

using ds::u16;
using ds::u32;
using ds::u8;

const BdHostFunctions kHost = {
    BD_GAME_LIBRARY_INTERFACE,
    static_cast<std::uint32_t>(sizeof(BdHostFunctions)),
    [](ds::Cpu* c, u32 a) -> u8 { return c->rd8(a); },
    [](ds::Cpu* c, u32 a) -> u16 { return c->rd16(a); },
    [](ds::Cpu* c, u32 a) -> u32 { return c->rd32(a); },
    [](ds::Cpu* c, u32 a, u8 v) { c->wr8(a, v); },
    [](ds::Cpu* c, u32 a, u16 v) { c->wr16(a, v); },
    [](ds::Cpu* c, u32 a, u32 v) { c->wr32(a, v); },
    [](ds::Cpu* c, u32 ret) -> bool { return ds::leave_slow(*c, ret); },
    [](ds::Cpu* c, u32 address) { ds::call(*c, address); },
    [](ds::Cpu* c, u32 address) { ds::jump(*c, address); },
    [](ds::Cpu* c, u32 number, u32 return_address) { return ds::swi(*c, number, return_address); },
    [](ds::Cpu* c, int cp, int opc1, int crn, int crm, int opc2, u32 value) {
      ds::cp_write(*c, cp, opc1, crn, crm, opc2, value);
    },
    [](ds::Cpu* c, int cp, int opc1, int crn, int crm, int opc2) -> u32 {
      return ds::cp_read(*c, cp, opc1, crn, crm, opc2);
    },
    [](ds::Cpu* c, u32 address, const char* what) { ds::unhandled(*c, address, what); },
    [](ds::Cpu* c, u32 next) -> bool { return ds::interrupt(*c, next); },
};

}  // namespace

const BdHostFunctions& host_functions() { return kHost; }

std::string check_game_library(const BdGameLibrary* library, const std::string& version) {
  if (library == nullptr) return "it is for another version of the program's interface";
  if (library->interface_version != BD_GAME_LIBRARY_INTERFACE) return "it is for another version of the interface";
  if (library->cpu_size != sizeof(ds::Cpu) || library->cpu_depth_offset != offsetof(ds::Cpu, depth) ||
      library->entry_size != sizeof(TranslatedEntry))
    return "it was compiled with another layout of the processor model";
  if (library->table == nullptr) return "it has no table of translated functions";
  const std::string built_for = library->built_for != nullptr ? library->built_for : "";
  if (!version.empty() && built_for != version)
    return "it was built for Bright Dawn " + (built_for.empty() ? std::string("(unknown)") : built_for) +
           ", not " + version;
  return "";
}

const char* GameLibrary::file_name() {
#if defined(_WIN32)
  return "bright-dawn-game.dll";
#else
  return "bright-dawn-game.so";
#endif
}

GameLibrary::~GameLibrary() { close(); }

void GameLibrary::close() {
  library_ = nullptr;
  if (handle_ == nullptr) return;
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
}

bool GameLibrary::open(const std::filesystem::path& path, const std::string& version) {
  close();
  error_.clear();
  void* entry = nullptr;
#if defined(_WIN32)
  HMODULE module = LoadLibraryW(path.c_str());
  if (module == nullptr) {
    error_ = "it could not be loaded (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  handle_ = module;
  entry = reinterpret_cast<void*>(GetProcAddress(module, BD_GAME_LIBRARY_ENTRY));
#else
  handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle_ == nullptr) {
    const char* why = dlerror();
    error_ = std::string("it could not be loaded: ") + (why != nullptr ? why : "unknown");
    return false;
  }
  entry = dlsym(handle_, BD_GAME_LIBRARY_ENTRY);
#endif
  if (entry == nullptr) {
    error_ = "it is not a game library: it has no " BD_GAME_LIBRARY_ENTRY;
    close();
    return false;
  }
  const BdGameLibrary* library = reinterpret_cast<BdGameLibraryEntry>(entry)(&kHost);
  if (std::string problem = check_game_library(library, version); !problem.empty()) {
    error_ = problem;
    close();
    return false;
  }
  library_ = library;
  return true;
}

}  // namespace bd::runtime
