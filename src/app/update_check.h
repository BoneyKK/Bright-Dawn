// The launcher's check for a newer Bright Dawn: the list of releases asked
// of GitHub's API by the system's curl (as app/fetch.h downloads), while
// the launcher goes on drawing, and read by ui/update_check.h. Nothing is
// sent but the request itself; nothing is downloaded but the list.
#pragma once

#include <memory>
#include <string>

#include "app/child_process.h"
#include "ui/update_check.h"

namespace bd::app {

class UpdateCheck {
 public:
  enum class State { Idle, Checking, Done, Failed };

  UpdateCheck() = default;
  UpdateCheck(const UpdateCheck&) = delete;
  UpdateCheck& operator=(const UpdateCheck&) = delete;

  // Starts asking; a check under way goes on.
  void start();
  // Moves it on, without waiting.
  void poll();

  State state() const { return state_; }
  // Once Done.
  const ui::UpdateResult& result() const { return result_; }
  // Once Failed: why, for people.
  const std::string& error() const { return error_; }

 private:
  void fail(const std::string& why);

  std::unique_ptr<ChildProcess> child_;
  std::string answer_;
  State state_ = State::Idle;
  ui::UpdateResult result_;
  std::string error_;
};

}  // namespace bd::app
