// "Report a problem": the address of the project's form for a bug report on
// GitHub, with what the program knows about this run already filled in, so
// that a player only has to say what happened and attach the log.
#pragma once

#include <string>
#include <string_view>

namespace bd::ui {

// The form for a new bug report (.github/ISSUE_TEMPLATE/bug.yml).
inline constexpr std::string_view kNewIssueUrl = "https://github.com/BoneyKK/Bright-Dawn/issues/new";
inline constexpr std::string_view kBugForm = "bug.yml";

// What is filled in; an empty field is left for the player. Each one is a
// field of the form, by its id.
struct ProblemReport {
  std::string version;    // "version": Bright Dawn's version and build.
  std::string os;         // "os": the operating system.
  std::string renderer;   // "renderer": what draws the window.
  std::string game_code;  // "game-code": BO5E, BO5P or BO5J.
  std::string code;       // "code": the built game or the interpreter.
};

// Percent-encodes everything but the characters an address keeps as they
// are (RFC 3986's unreserved ones); UTF-8 byte by byte.
std::string percent_encode(std::string_view text);

// The bug form's address with the report's fields filled in.
std::string problem_report_url(const ProblemReport& report);

}  // namespace bd::ui
