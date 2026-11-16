#include "app/hang_watch.h"

#include <cinttypes>

#include "core/log.h"

namespace bd::app {
namespace {

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

HangWatch::HangWatch(const std::atomic<std::uint32_t>& guest, std::chrono::seconds limit)
    : guest_(guest), limit_(limit), thread_([this] { watch(); }) {}

HangWatch::~HangWatch() {
  {
    std::lock_guard lock(mutex_);
    ending_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void HangWatch::resume() { running_since_.store(now_ns()); }

void HangWatch::pause() { running_since_.store(0); }

void HangWatch::watch() {
  std::int64_t reported = 0;  // The resume time of the stretch reported as stuck.
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait_for(lock, std::chrono::seconds(1), [this] { return ending_; });
    if (ending_) return;
    const std::int64_t since = running_since_.load();
    if (reported != 0 && since != reported) {
      log::writef(log::Level::Warning, "game", "the game went on after %.0f s without a frame",
                  static_cast<double>((since != 0 ? since : now_ns()) - reported) / 1e9);
      reported = 0;
    }
    if (since == 0 || reported != 0) continue;
    const std::int64_t limit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(limit_).count();
    if (now_ns() - since < limit_ns) continue;
    reported = since;
    log::writef(log::Level::Warning, "game",
                "the game has not finished a frame for %lld s: it may be stuck; its code was last entered at "
                "0x%08" PRIX32,
                static_cast<long long>(limit_.count()), guest_.load(std::memory_order_relaxed));
  }
}

}  // namespace bd::app
