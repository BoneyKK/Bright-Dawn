#include "app/fetch.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <system_error>

#include "core/log.h"
#include "core/sha256.h"

namespace bd::app {
namespace {

// A program of the system's own, from System32 on Windows.
std::string system_program(const char* name) {
#if defined(_WIN32)
  const char* root = SDL_getenv("SystemRoot");
  return std::string(root != nullptr ? root : "C:\\Windows") + "\\System32\\" + name + ".exe";
#else
  return name;
#endif
}

}  // namespace

Fetch::~Fetch() {
  stop();
  remove();
}

bool Fetch::start(const Download& download, std::string& error) {
  download_ = download;
  error_.clear();
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    error = "no temporary folder: " + ec.message();
    return false;
  }
  for (int n = 0;; ++n) {
    folder_ = base / ("bright-dawn-fetch-" + std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(n));
    if (std::filesystem::create_directory(folder_, ec)) break;
    if (n > 100) {
      error = "cannot make a temporary folder in " + utf8(base);
      folder_.clear();
      return false;
    }
  }
  file_ = folder_ / "download";
  child_ = std::make_unique<ChildProcess>();
  const std::vector<std::string> args = {system_program("curl"), "--location", "--fail", "--silent", "--show-error",
                                         "--retry", "2", "--output", utf8(file_), download.url};
  log::info("fetch", download.what + ": " + download.url + " into " + utf8(folder_));
  if (!child_->start(args, false, error)) {
    error = "the download could not be started (" + args[0] + "): " + error;
    child_.reset();
    return false;
  }
  state_ = State::Downloading;
  return true;
}

void Fetch::fail(const std::string& why) {
  error_ = why;
  state_ = State::Failed;
  log::warning("fetch", download_.what + ": " + why);
  child_.reset();
}

void Fetch::poll(const std::function<void(std::string_view)>& line) {
  if (child_ == nullptr) return;
  child_->poll(line);
  if (!child_->ended()) return;
  const int code = child_->exit_code();
  child_.reset();
  if (state_ == State::Downloading) {
    if (ChildProcess::kExitCodeKnown && code != 0) {
      fail("the download failed (curl exit status " + std::to_string(code) + ")");
      return;
    }
    const auto sha = sha256_of_file(file_);
    if (!sha) {
      fail("the download failed: nothing was received");
      return;
    }
    if (*sha != download_.sha256) {
      fail("the download is not what was expected (SHA-256 " + *sha + "); try again");
      return;
    }
    line(download_.what + ": downloaded, and its SHA-256 is the one expected");
    child_ = std::make_unique<ChildProcess>();
    const std::vector<std::string> args = {system_program("tar"), "-x", "-f", utf8(file_), "-C", utf8(folder_)};
    std::string error;
    if (!child_->start(args, false, error)) {
      fail("it could not be unpacked (" + args[0] + "): " + error);
      return;
    }
    state_ = State::Unpacking;
    return;
  }
  if (state_ == State::Unpacking) {
    std::error_code ec;
    if ((ChildProcess::kExitCodeKnown && code != 0) || !std::filesystem::is_regular_file(program(), ec)) {
      fail("it could not be unpacked (tar exit status " + std::to_string(code) + ")");
      return;
    }
    std::filesystem::remove(file_, ec);
    line(download_.what + ": ready, for this build only");
    log::info("fetch", download_.what + ": ready: " + utf8(program()));
    state_ = State::Done;
  }
}

void Fetch::stop() {
  if (child_ != nullptr) {
    child_->kill();
    for (int i = 0; i < 200 && !child_->ended(); ++i) {
      child_->poll([](std::string_view) {});
      SDL_Delay(10);
    }
    child_.reset();
  }
  if (state_ == State::Downloading || state_ == State::Unpacking) {
    state_ = State::Failed;
    error_ = "stopped";
  }
}

void Fetch::remove() {
  if (folder_.empty()) return;
  std::error_code ec;
  // On Windows a file can stay in use for a moment after its program ended.
  for (int i = 0; i < 20; ++i) {
    std::filesystem::remove_all(folder_, ec);
    if (!std::filesystem::exists(folder_, ec)) break;
    SDL_Delay(100);
  }
  if (std::filesystem::exists(folder_, ec)) log::warning("fetch", "could not remove " + utf8(folder_));
  else log::info("fetch", "removed " + utf8(folder_));
  folder_.clear();
}

}  // namespace bd::app
