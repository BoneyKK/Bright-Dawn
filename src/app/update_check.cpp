#include "app/update_check.h"

#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/version.h"

namespace bd::app {
namespace {

// Long enough for a slow connection; the list is some tens of kilobytes.
constexpr const char* kTimeoutSeconds = "20";

}  // namespace

void UpdateCheck::start() {
  if (state_ == State::Checking) return;
  answer_.clear();
  error_.clear();
  result_ = {};
  child_ = std::make_unique<ChildProcess>();
  // GitHub's API asks for a User-Agent; this one names the program only.
  const std::vector<std::string> args = {system_program("curl"),
                                         "--location",
                                         "--fail",
                                         "--silent",
                                         "--max-time",
                                         kTimeoutSeconds,
                                         "--header",
                                         "Accept: application/vnd.github+json",
                                         "--user-agent",
                                         "Bright-Dawn-launcher",
                                         std::string(ui::kReleasesApi)};
  std::string error;
  if (!child_->start(args, false, error)) {
    child_.reset();
    fail("curl could not be started (" + args[0] + "): " + error);
    return;
  }
  log::info("update", "asking GitHub for the releases");
  state_ = State::Checking;
}

void UpdateCheck::fail(const std::string& why) {
  error_ = why;
  state_ = State::Failed;
  log::warning("update", "the check for a newer version failed: " + why);
}

void UpdateCheck::poll() {
  if (child_ == nullptr) return;
  child_->poll([this](std::string_view line) {
    answer_.append(line);
    answer_ += '\n';
  });
  if (!child_->ended()) return;
  const int code = child_->exit_code();
  child_.reset();
  if (ChildProcess::kExitCodeKnown && code != 0) {
    fail("no answer from GitHub (curl exit status " + std::to_string(code) + ")");
    return;
  }
  result_ = ui::compare_releases(BD_VERSION, answer_);
  switch (result_.kind) {
    case ui::UpdateResult::Unreadable: fail("GitHub's answer could not be read"); return;
    case ui::UpdateResult::NoRelease: log::info("update", "no release is out yet"); break;
    case ui::UpdateResult::UpToDate: log::info("update", "up to date (newest: " + result_.latest.tag + ")"); break;
    case ui::UpdateResult::Newer: log::info("update", result_.latest.tag + " is out: " + result_.latest.page); break;
    case ui::UpdateResult::NotRelease:
      log::info("update", "a build from the source tree; the newest release is " + result_.latest.tag);
      break;
  }
  state_ = State::Done;
}

}  // namespace bd::app
