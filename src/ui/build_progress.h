// Following the game builder (tools/gamebuilder/build_game.py) by its
// output: which step it is at, how far into that step, how it ended. Kept
// apart from the window that shows it, so that it can be tested.
//
// The builder writes, among the output of the programs it runs:
//   == 2/5. Checking the ROM          a step begins
//   error: <what went wrong>          it failed
//   The game: <path>                  it succeeded, and the game is there
// and the build programs write how far they are: Ninja "[12/345] ...",
// Make "[ 45%] ...".
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace bd::ui {

class BuildProgress {
 public:
  // One line of output, without its line end.
  void take_line(std::string_view line);

  int step() const { return step_; }    // From 1; 0 before the first.
  int steps() const { return steps_; }  // 0 until the first step is announced.
  const std::string& step_name() const { return step_name_; }
  // How far into the step, 0 to 1, when the step's programs say; else nothing.
  std::optional<double> within_step() const;
  // How far the whole build is, 0 to 1.
  double fraction() const;
  // The builder's own error message, if it gave one.
  const std::string& error() const { return error_; }
  // Where the game was built, once it says.
  const std::string& game() const { return game_; }

 private:
  int step_ = 0, steps_ = 0;
  std::string step_name_;
  long long done_ = -1, total_ = -1;
  std::string error_, game_;
};

// The first version number in a program's report of its version
// ("Python 3.12.1", "cmake version 3.28.3"): major, minor, patch (0 if not
// given). Nothing if there is none.
std::optional<std::array<int, 3>> version_in(std::string_view text);

}  // namespace bd::ui
