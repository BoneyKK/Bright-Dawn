// Other programs run by the window program: the game builder, the tools it
// needs (to check that they are there), the built game. Their standard error
// goes with their standard output, which is read without waiting, a line at
// a time.
//
// On Windows they are started without a console window of their own (the
// builder's programs then share its hidden one), which SDL does only for
// programs started "in the background"; their exit code is then not known.
// What they print has to say how they ended.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bd::app {

class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  // A program still running is ended.
  ~ChildProcess();

  // Starts `args` (the program, then its arguments; the program is looked
  // for on the PATH). With `input`, its standard input is a pipe from here,
  // which close_input() closes; otherwise it has none.
  bool start(const std::vector<std::string>& args, bool input, std::string& error);
  // Reads what it has written, giving each complete line (without its line
  // end) to `line`; once it has ended, the rest too.
  void poll(const std::function<void(std::string_view)>& line);
  bool running() const { return process_ != nullptr && !ended_; }
  bool ended() const { return ended_; }
  // Its exit code, once it has ended, where it is known (not on Windows).
  int exit_code() const { return exit_code_; }
  static constexpr bool kExitCodeKnown =
#if defined(_WIN32)
      false;
#else
      true;
#endif
  void close_input();
  // Ends it at once.
  void kill();

 private:
  void read_output(const std::function<void(std::string_view)>& line);

  SDL_Process* process_ = nullptr;
  std::string partial_;
  bool ended_ = false;
  int exit_code_ = -1;
};

// Runs a program to its end, for at most `timeout_ms`, and returns what it
// wrote. `started` is false if it could not be started; `exit_code` is -1
// if it had to be ended (and 0 where it is not known).
struct ProgramOutput {
  bool started = false;
  int exit_code = -1;
  std::string output;
};
ProgramOutput run_program(const std::vector<std::string>& args, std::uint32_t timeout_ms);

// A program of the system's own: from System32 on Windows (so that no other
// program of the same name is used), else looked for on the PATH. Used for
// curl and tar, part of Windows 10 (from 1803) and 11.
std::string system_program(const char* name);

}  // namespace bd::app
