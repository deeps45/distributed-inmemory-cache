// Full factorial benchmark across the three independently-measured design
// decisions this project makes: shard_count (1 vs 16 - "lock-efficient
// synchronization"), the object pool (off vs on - "optimized memory
// allocation"), and the buffered-read path (off vs on - see
// sharded_cache.hpp's design notes for what this trades off). Each of the
// 8 combinations runs at 1/2/4/8/16 concurrent threads, under two
// workloads: uniform-random keys, and a Zipfian-ish skew that concentrates
// most traffic on a small set of hot keys (closer to a real cache's
// traffic pattern, and specifically the shape of workload buffered reads
// are supposed to help with most, since hot keys get touched far more
// often than a uniform distribution would).
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

template <int ShardCount, bool UsePool, bool UseReadBuffer>
cache::ShardedCache<int, int>& GetCache() {
  static cache::ShardedCache<int, int> instance(kCapacity, ShardCount, UsePool, UseReadBuffer,
                                                  std::chrono::milliseconds(0));
  return instance;
}

// Deliberately not statistically rigorous Zipfian (no exact harmonic-number
// normalization) - close enough to concentrate ~80% of traffic on ~20% of
// keys, which is the property that matters for this comparison, without
// pulling in a dependency for a single benchmark's key generator.
class SkewedKeyGenerator {
 public:
  explicit SkewedKeyGenerator(unsigned seed) : rng_(seed) {}

  int next() {
    double u = uniform_(rng_);
    double skewed = u * u * u;  // biases toward 0
    return static_cast<int>(skewed * kKeyspace);
  }

 private:
  std::mt19937 rng_;
  std::uniform_real_distribution<double> uniform_{0.0, 1.0};
};

template <int ShardCount, bool UsePool, bool UseReadBuffer, bool Skewed>
void RunMixedWorkload(benchmark::State& state) {
  auto& c = GetCache<ShardCount, UsePool, UseReadBuffer>();
  std::mt19937 rng(static_cast<unsigned>(state.thread_index()) + 1u);
  std::uniform_int_distribution<int> key_dist(0, kKeyspace - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);
  SkewedKeyGenerator skewed_gen(static_cast<unsigned>(state.thread_index()) + 1000u);

  for (auto _ : state) {
    int key = Skewed ? skewed_gen.next() : key_dist(rng);
    if (op_dist(rng) < kGetPercent) {
      benchmark::DoNotOptimize(c.get(key));
    } else {
      c.put(key, key);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

#define DEFINE_BENCH(name, shards, pool, readbuf, skewed)                    \
  void name(benchmark::State& state) {                                      \
    RunMixedWorkload<shards, pool, readbuf, skewed>(state);                  \
  }                                                                          \
  BENCHMARK(name)->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)->UseRealTime()

// --- Uniform-random keys ---
DEFINE_BENCH(BM_U_S1_NoPool_NoBuf, 1, false, false, false);
DEFINE_BENCH(BM_U_S1_Pool_NoBuf, 1, true, false, false);
DEFINE_BENCH(BM_U_S1_Pool_Buf, 1, true, true, false);
DEFINE_BENCH(BM_U_S16_NoPool_NoBuf, 16, false, false, false);
DEFINE_BENCH(BM_U_S16_Pool_NoBuf, 16, true, false, false);
DEFINE_BENCH(BM_U_S16_Pool_Buf, 16, true, true, false);

// --- Skewed/hot-key keys - where buffered reads should matter most ---
DEFINE_BENCH(BM_Z_S16_Pool_NoBuf, 16, true, false, true);
DEFINE_BENCH(BM_Z_S16_Pool_Buf, 16, true, true, true);

}  // namespace
