// Loading the game library (game_library.h) into the program, and checking
// it before anything of it is used.
#pragma once

#include <filesystem>
#include <string>

#include "game_library.h"

namespace bd::runtime {

// The program's functions, as the library calls them: the ones the runtime
// defines for translated code linked into the program (machine.cpp).
const BdHostFunctions& host_functions();

// Whether a library's answer can be used by this program, which expects
// libraries built for `version` (empty: any). Returns what is wrong, or an
// empty text.
std::string check_game_library(const BdGameLibrary* library, const std::string& version);

class GameLibrary {
 public:
  GameLibrary() = default;
  GameLibrary(const GameLibrary&) = delete;
  GameLibrary& operator=(const GameLibrary&) = delete;
  ~GameLibrary();

  // Loads the library at `path` and checks it (check_game_library). On
  // failure, error() says why and nothing stays loaded.
  bool open(const std::filesystem::path& path, const std::string& version);
  void close();

  bool is_open() const { return library_ != nullptr; }
  // The translated functions; valid while it is open.
  const TranslatedEntry* table() const { return library_ != nullptr ? library_->table : nullptr; }
  const char* built_for() const { return library_ != nullptr ? library_->built_for : ""; }
  const std::string& error() const { return error_; }

  // The library's file name on this system: "bright-dawn-game.dll" or
  // "bright-dawn-game.so".
  static const char* file_name();

 private:
  void* handle_ = nullptr;
  const BdGameLibrary* library_ = nullptr;
  std::string error_;
};

}  // namespace bd::runtime
