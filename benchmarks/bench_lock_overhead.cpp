// Isolates the specific claim benchmarks/bench_cache.cpp's read-buffer
// comparison rests on: that std::shared_mutex has meaningfully higher
// fixed overhead than std::mutex for a short critical section, even with
// zero contention (single thread). This is what's actually responsible
// for the read-buffer mode measuring slower overall - not a flaw in the
// buffering logic itself. See sharded_cache.hpp's design notes and the
// README's "Buffered reads: measured, and rejected" section.
#include <mutex>
#include <shared_mutex>

#include <benchmark/benchmark.h>

namespace {

void BM_StdMutex_LockUnlock(benchmark::State& state) {
  std::mutex m;
  int sink = 0;
  for (auto _ : state) {
    std::lock_guard<std::mutex> lock(m);
    benchmark::DoNotOptimize(sink += 1);
  }
}
BENCHMARK(BM_StdMutex_LockUnlock);

void BM_SharedMutex_SharedLockUnlock(benchmark::State& state) {
  std::shared_mutex m;
  int sink = 0;
  for (auto _ : state) {
    std::shared_lock<std::shared_mutex> lock(m);
    benchmark::DoNotOptimize(sink += 1);
  }
}
BENCHMARK(BM_SharedMutex_SharedLockUnlock);

void BM_SharedMutex_ExclusiveLockUnlock(benchmark::State& state) {
  std::shared_mutex m;
  int sink = 0;
  for (auto _ : state) {
    std::lock_guard<std::shared_mutex> lock(m);
    benchmark::DoNotOptimize(sink += 1);
  }
}
BENCHMARK(BM_SharedMutex_ExclusiveLockUnlock);

}  // namespace
