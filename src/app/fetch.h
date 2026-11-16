// Fetching what a build needs but the computer lacks (Python, on Windows):
// downloaded into a temporary folder of its own, its SHA-256 checked, and
// unpacked there, while the launcher goes on drawing. The folder is removed
// with the Fetch, or by remove().
//
// The system's own programs do the work: curl to download and tar to
// unpack, both part of Windows 10 (from 1803) and 11, taken from System32
// so that no other program of the same name is used.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "app/build_tools.h"
#include "app/child_process.h"

namespace bd::app {

class Fetch {
 public:
  enum class State { Idle, Downloading, Unpacking, Done, Failed };

  Fetch() = default;
  Fetch(const Fetch&) = delete;
  Fetch& operator=(const Fetch&) = delete;
  ~Fetch();

  // Starts fetching into a new temporary folder.
  bool start(const Download& download, std::string& error);
  // Moves it on; what the programs print goes to `line`.
  void poll(const std::function<void(std::string_view)>& line);
  // Ends it where it is: the state becomes Failed.
  void stop();
  // Removes the folder and what is in it.
  void remove();

  State state() const { return state_; }
  const std::string& error() const { return error_; }
  // The program fetched, once Done.
  std::filesystem::path program() const { return folder_ / download_.program; }
  const std::filesystem::path& folder() const { return folder_; }

 private:
  void fail(const std::string& why);

  Download download_;
  std::filesystem::path folder_;
  std::filesystem::path file_;
  std::unique_ptr<ChildProcess> child_;
  State state_ = State::Idle;
  std::string error_;
};

}  // namespace bd::app
