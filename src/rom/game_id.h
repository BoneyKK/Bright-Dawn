// Which retail releases this project recognises as Golden Sun: Dark Dawn.
#pragma once

#include <span>
#include <string_view>

#include "core/result.h"
#include "rom/nds_header.h"

namespace bd::rom {

enum class Region { Usa, Europe, Japan };

const char* to_string(Region region);

struct KnownTitle {
  std::string_view game_code;
  Region region;
};

// Releases identified by cartridge game code.
std::span<const KnownTitle> known_titles();

// Returns the matching release, or nullptr when the header belongs to some
// other software.
const KnownTitle* identify_title(const NdsHeader& header);

// Fails with WrongGame, and an explanation suitable for showing to the user,
// when the header is not a recognised release of the game.
Status require_dark_dawn(const NdsHeader& header);

}  // namespace bd::rom
