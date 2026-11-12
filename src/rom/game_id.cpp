#include "rom/game_id.h"

#include <array>
#include <string>

namespace bd::rom {
namespace {

constexpr std::array<KnownTitle, 3> kTitles{{
    {"BO5E", Region::Usa},
    {"BO5P", Region::Europe},
    {"BO5J", Region::Japan},
}};

// Game codes come from the ROM; keep anything unprintable out of messages.
std::string printable(std::string_view text) {
  std::string out;
  for (char c : text) out += (c >= 0x20 && c < 0x7F) ? c : '?';
  return out;
}

}  // namespace

const char* to_string(Region region) {
  switch (region) {
    case Region::Usa: return "USA";
    case Region::Europe: return "Europe";
    case Region::Japan: return "Japan";
  }
  return "unknown";
}

std::span<const KnownTitle> known_titles() { return kTitles; }

const KnownTitle* identify_title(const NdsHeader& header) {
  for (const KnownTitle& title : kTitles) {
    if (header.game_code == title.game_code) return &title;
  }
  return nullptr;
}

Status require_dark_dawn(const NdsHeader& header) {
  if (identify_title(header) != nullptr) return {};
  std::string expected;
  for (const KnownTitle& title : kTitles) {
    if (!expected.empty()) expected += ", ";
    expected += std::string(title.game_code) + " (" + to_string(title.region) + ")";
  }
  return Error{ErrorCode::WrongGame,
               "this is a valid Nintendo DS ROM, but not Golden Sun: Dark Dawn. It identifies "
               "itself as \"" + printable(header.title) + "\" with game code " +
                   printable(header.game_code) + "; expected one of: " + expected};
}

}  // namespace bd::rom
