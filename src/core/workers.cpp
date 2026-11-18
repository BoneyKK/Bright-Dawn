#include "core/workers.h"

#include <algorithm>

namespace bd::core {

Workers::Workers(unsigned threads) : wanted_(threads) {}

Workers::~Workers() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  start_.notify_all();
  for (std::thread& thread : threads_) thread.join();
}

unsigned Workers::default_threads() {
  const unsigned processors = std::thread::hardware_concurrency();
  return processors <= 1 ? 0 : std::min(processors - 1, 7u);
}

void Workers::for_each(std::size_t count, const std::function<void(std::size_t)>& job) {
  if (wanted_ == 0 || count <= 1) {
    for (std::size_t i = 0; i < count; ++i) job(i);
    return;
  }
  if (threads_.empty()) {
    threads_.reserve(wanted_);
    for (unsigned i = 0; i < wanted_; ++i) threads_.emplace_back([this] { work(); });
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = &job;
    count_ = count;
    next_.store(0, std::memory_order_relaxed);
    busy_ = static_cast<unsigned>(threads_.size());
    ++round_;
  }
  start_.notify_all();
  take_jobs();
  // Every thread finishes the round, even one that found nothing left, so
  // that none is still in it when the next one begins.
  std::unique_lock<std::mutex> lock(mutex_);
  done_.wait(lock, [this] { return busy_ == 0; });
  job_ = nullptr;
}

void Workers::take_jobs() {
  for (std::size_t i = next_.fetch_add(1); i < count_; i = next_.fetch_add(1)) (*job_)(i);
}

void Workers::work() {
  std::uint64_t seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      start_.wait(lock, [&] { return stopping_ || round_ != seen; });
      if (stopping_) return;
      seen = round_;
    }
    take_jobs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (--busy_ == 0) done_.notify_one();
    }
  }
}

}  // namespace bd::core
