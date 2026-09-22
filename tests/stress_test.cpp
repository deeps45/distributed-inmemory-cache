// Deliberately adversarial concurrency test, meant to be run under
// ThreadSanitizer (see Makefile's `tsan` target / CI). Many threads, a
// small keyspace relative to thread count (to maximize same-key
// contention within and across shards), and a mix of get/put/erase -
// erase() in particular is the operation most likely to race with a
// concurrent get() on the same key if the shard locking were ever
// loosened incorrectly.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache/sharded_cache.hpp"

int main() {
  constexpr int kThreads = 32;
  constexpr int kOpsPerThread = 50000;
  constexpr int kKeyspace = 64;  // small on purpose - forces heavy overlap
  constexpr int kShards = 8;

  cache::ShardedCache<int, std::string> c(/*capacity=*/kKeyspace / 2, kShards, /*use_pool=*/true,
                                           std::chrono::milliseconds(20));

  std::atomic<long> total_ops{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      unsigned seed = static_cast<unsigned>(t) * 7919u + 17u;
      auto rnd = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (seed / 65536u) % 32768u;
      };
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key = static_cast<int>(rnd() % kKeyspace);
        int op = static_cast<int>(rnd() % 10);
        if (op < 6) {
          c.get(key);
        } else if (op < 9) {
          c.put(key, "thread-" + std::to_string(t) + "-op-" + std::to_string(i),
                (i % 5 == 0) ? std::optional(std::chrono::milliseconds(5)) : std::nullopt);
        } else {
          c.erase(key);
        }
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& th : threads) th.join();

  auto stats = c.stats();
  std::cout << "total_ops=" << total_ops.load() << " final_size=" << c.size()
            << " hits=" << stats.hits << " misses=" << stats.misses
            << " evictions=" << stats.evictions << " expirations=" << stats.expirations << "\n";

  if (c.size() > kKeyspace / 2) {
    std::cerr << "INVARIANT VIOLATED: cache size " << c.size() << " exceeds capacity "
              << kKeyspace / 2 << "\n";
    return 1;
  }
  std::cout << "stress test passed\n";
  return 0;
}
