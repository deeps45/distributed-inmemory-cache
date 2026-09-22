// 2x2 factorial benchmark: shard_count (1 vs 16) x object-pool (off vs on),
// each run at 1/2/4/8/16 concurrent threads. This isolates the two
// resume-bullet claims - "lock-efficient synchronization" (sharding) and
// "optimized memory allocation" (the pool) - as independently measured
// variables rather than one bundled "it's faster" number.
//
// A function-local static holds each configuration's cache instance:
// C++11 guarantees thread-safe first-call construction ("magic statics"),
// and every thread Google Benchmark spawns for a given ->Threads(N) call
// invokes the same function, so they all share the same instance - exactly
// the setup a real concurrent cache benchmark needs.
#include <chrono>
#include <cstdlib>
#include <random>

#include <benchmark/benchmark.h>

#include "cache/sharded_cache.hpp"

namespace {

constexpr int kKeyspace = 10000;
constexpr int kCapacity = 2000;
constexpr int kGetPercent = 90;  // 90/10 read/write - a common cache workload ratio (cf. YCSB workload B)

template <int ShardCount, bool UsePool>
cache::ShardedCache<int, int>& GetCache() {
  static cache::ShardedCache<int, int> instance(kCapacity, ShardCount, UsePool,
                                                  std::chrono::milliseconds(0));
  return instance;
}

template <int ShardCount, bool UsePool>
void RunMixedWorkload(benchmark::State& state) {
  auto& c = GetCache<ShardCount, UsePool>();
  std::mt19937 rng(static_cast<unsigned>(state.thread_index()) + 1u);
  std::uniform_int_distribution<int> key_dist(0, kKeyspace - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (auto _ : state) {
    int key = key_dist(rng);
    if (op_dist(rng) < kGetPercent) {
      benchmark::DoNotOptimize(c.get(key));
    } else {
      c.put(key, key);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

void BM_GlobalLock_NoPool(benchmark::State& state) { RunMixedWorkload<1, false>(state); }
void BM_GlobalLock_WithPool(benchmark::State& state) { RunMixedWorkload<1, true>(state); }
void BM_Sharded16_NoPool(benchmark::State& state) { RunMixedWorkload<16, false>(state); }
void BM_Sharded16_WithPool(benchmark::State& state) { RunMixedWorkload<16, true>(state); }

}  // namespace

#define THREAD_SWEEP ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)->UseRealTime()

BENCHMARK(BM_GlobalLock_NoPool) THREAD_SWEEP;
BENCHMARK(BM_GlobalLock_WithPool) THREAD_SWEEP;
BENCHMARK(BM_Sharded16_NoPool) THREAD_SWEEP;
BENCHMARK(BM_Sharded16_WithPool) THREAD_SWEEP;
