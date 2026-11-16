// Notices a game that has stopped finishing frames: a hang inside the
// emulation, which otherwise leaves nothing in the log before the program is
// closed. A thread of its own looks once a second; after `limit` without a
// frame it logs where the game's code was, and again when it goes on.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bd::app {

class HangWatch {
 public:
  // `guest` is where the game's code was last entered (see
  // runtime::Machine::last_entered); it must outlive the watch.
  HangWatch(const std::atomic<std::uint32_t>& guest, std::chrono::seconds limit);
  ~HangWatch();
  HangWatch(const HangWatch&) = delete;
  HangWatch& operator=(const HangWatch&) = delete;

  // The emulation runs from now on, until paused.
  void resume();
  // The program does its own work (showing a frame, a menu): no frame is due.
  void pause();

 private:
  void watch();

  const std::atomic<std::uint32_t>& guest_;
  const std::chrono::seconds limit_;
  // When the emulation last resumed, in steady-clock nanoseconds; 0 while
  // paused.
  std::atomic<std::int64_t> running_since_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool ending_ = false;
  std::thread thread_;
};

}  // namespace bd::app
