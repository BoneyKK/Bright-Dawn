// The crew of threads that shares out numbered jobs (the 3D lines).
#include <atomic>
#include <cstddef>
#include <vector>

#include "core/workers.h"
#include "test.h"

using bd::core::Workers;

TEST(workers_call_every_job_once) {
  for (unsigned threads : {0u, 1u, 3u, 7u}) {
    Workers workers(threads);
    for (int round = 0; round < 50; ++round) {
      std::vector<std::atomic<int>> calls(192);
      workers.for_each(calls.size(), [&](std::size_t i) { calls[i].fetch_add(1); });
      bool once = true;
      for (const std::atomic<int>& c : calls) once = once && c.load() == 1;
      CHECK(once);
    }
  }
}

TEST(workers_give_the_same_result_as_one_thread) {
  // Each job writes only its own entry, from data no job changes.
  std::vector<std::size_t> input(1000), alone(1000), together(1000);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = i * 2654435761u;
  const auto job = [&](std::vector<std::size_t>& out) {
    return [&input, &out](std::size_t i) { out[i] = input[i] ^ (input[(i + 1) % input.size()] >> 3); };
  };
  Workers one(0), many(5);
  one.for_each(input.size(), job(alone));
  many.for_each(input.size(), job(together));
  CHECK(alone == together);
}

TEST(workers_handle_no_jobs_and_one_job) {
  Workers workers(3);
  int calls = 0;
  workers.for_each(0, [&](std::size_t) { ++calls; });
  CHECK(calls == 0);
  workers.for_each(1, [&](std::size_t i) { calls += static_cast<int>(i) + 1; });
  CHECK(calls == 1);
  CHECK(Workers::default_threads() <= 7);
}
