// A crew of threads that share out a numbered list of jobs.
//
// for_each(count, job) calls job(i) once for every i from 0 to count - 1, on
// the calling thread and on the crew's threads at once, each taking the next
// number not yet taken, and returns when all are done. The jobs must not
// depend on each other or touch the same data, so that the result is the
// same whatever thread does which, and in whatever order: the same as
// calling them one after another.
//
// The threads are started at the first for_each and wait, asleep, between
// calls. A crew of no threads calls every job on the calling thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bd::core {

class Workers {
 public:
  // `threads`: besides the calling one.
  explicit Workers(unsigned threads = default_threads());
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;
  ~Workers();

  void for_each(std::size_t count, const std::function<void(std::size_t)>& job);
  unsigned threads() const { return wanted_; }

  // One fewer than the processors this computer has, and at most 7: the
  // calling thread makes the number up.
  static unsigned default_threads();

 private:
  void work();
  void take_jobs();

  const unsigned wanted_;
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable start_, done_;
  const std::function<void(std::size_t)>* job_ = nullptr;
  std::size_t count_ = 0;
  std::atomic<std::size_t> next_{0};
  unsigned busy_ = 0;
  std::uint64_t round_ = 0;
  bool stopping_ = false;
};

}  // namespace bd::core
