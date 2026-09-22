#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cache/sharded_cache.hpp"

using cache::ShardedCache;

TEST(BasicOps, PutThenGetReturnsValue) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 42);
  auto v = c.get("a");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 42);
}

TEST(BasicOps, GetMissingKeyReturnsNullopt) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  EXPECT_FALSE(c.get("nope").has_value());
}

TEST(BasicOps, PutOverwritesExistingKey) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  c.put("a", 2);
  EXPECT_EQ(*c.get("a"), 2);
  EXPECT_EQ(c.size(), 1u);
}

TEST(BasicOps, EraseRemovesKey) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  EXPECT_TRUE(c.erase("a"));
  EXPECT_FALSE(c.get("a").has_value());
  EXPECT_FALSE(c.erase("a"));  // second erase: nothing there
}

TEST(Lru, EvictsLeastRecentlyUsedOnOverflow) {
  ShardedCache<std::string, int> c(2, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  c.put("b", 2);
  c.put("c", 3);  // capacity 2 -> "a" (LRU) evicted
  EXPECT_FALSE(c.get("a").has_value());
  EXPECT_TRUE(c.get("b").has_value());
  EXPECT_TRUE(c.get("c").has_value());
}

TEST(Lru, GetRefreshesRecency) {
  ShardedCache<std::string, int> c(2, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  c.put("b", 2);
  c.get("a");     // "a" now MRU, "b" now LRU
  c.put("c", 3);  // should evict "b", not "a"
  EXPECT_TRUE(c.get("a").has_value());
  EXPECT_FALSE(c.get("b").has_value());
  EXPECT_TRUE(c.get("c").has_value());
}

TEST(Lru, PutOnExistingKeyRefreshesRecency) {
  ShardedCache<std::string, int> c(2, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  c.put("b", 2);
  c.put("a", 10);  // touches "a" -> "b" becomes LRU
  c.put("c", 3);   // evicts "b"
  EXPECT_TRUE(c.get("a").has_value());
  EXPECT_FALSE(c.get("b").has_value());
}

TEST(Lru, RespectsCapacityUnderRepeatedInserts) {
  ShardedCache<int, int> c(5, 1, true, false, std::chrono::milliseconds(0));
  for (int i = 0; i < 100; ++i) c.put(i, i);
  EXPECT_EQ(c.size(), 5u);
  // Only the last 5 keys inserted should survive.
  for (int i = 95; i < 100; ++i) EXPECT_TRUE(c.get(i).has_value()) << "key " << i;
  for (int i = 0; i < 95; ++i) EXPECT_FALSE(c.get(i).has_value()) << "key " << i;
}

TEST(Ttl, ExpiresAfterDuration) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1, std::chrono::milliseconds(20));
  EXPECT_TRUE(c.get("a").has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(c.get("a").has_value());
}

TEST(Ttl, NoTtlNeverExpiresOnItsOwn) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);  // no TTL
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(c.get("a").has_value());
}

TEST(Ttl, BackgroundReaperEvictsWithoutBeingRead) {
  ShardedCache<std::string, int> c(10, 1, true, false, std::chrono::milliseconds(10));
  c.put("a", 1, std::chrono::milliseconds(15));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // several reap cycles
  EXPECT_EQ(c.stats().expirations, 1u);
  EXPECT_EQ(c.size(), 0u);
}

TEST(Stats, TracksHitsMissesEvictions) {
  ShardedCache<std::string, int> c(1, 1, true, false, std::chrono::milliseconds(0));
  c.put("a", 1);
  c.get("a");        // hit
  c.get("missing");  // miss
  c.put("b", 2);      // evicts "a"
  auto s = c.stats();
  EXPECT_EQ(s.hits, 1u);
  EXPECT_EQ(s.misses, 1u);
  EXPECT_EQ(s.evictions, 1u);
}

TEST(Sharding, ShardCountOneBehavesAsGlobalLockBaseline) {
  ShardedCache<int, int> c(100, 1, true, false, std::chrono::milliseconds(0));
  EXPECT_EQ(c.shard_count(), 1u);
  for (int i = 0; i < 50; ++i) c.put(i, i * i);
  for (int i = 0; i < 50; ++i) EXPECT_EQ(*c.get(i), i * i);
}

TEST(Sharding, DistributesKeysAcrossShards) {
  ShardedCache<int, int> c(1000, 16, true, false, std::chrono::milliseconds(0));
  for (int i = 0; i < 500; ++i) c.put(i, i);
  EXPECT_EQ(c.size(), 500u);
  for (int i = 0; i < 500; ++i) EXPECT_EQ(*c.get(i), i);
}

TEST(Concurrency, ManyThreadsNeverExceedCapacity) {
  constexpr int kCapacity = 200;
  ShardedCache<int, int> c(kCapacity, 8, true, false, std::chrono::milliseconds(0));
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&c, t] {
      for (int i = 0; i < 20000; ++i) {
        int key = (t * 20000 + i) % 1000;
        c.put(key, key);
        c.get(key);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_LE(c.size(), static_cast<std::size_t>(kCapacity));
}

TEST(ReadBuffer, ExplicitlyDisabledStillBehavesCorrectly) {
  // false is the default (see sharded_cache.hpp for why: buffered reads
  // measured slower on this project's benchmarks and aren't recommended),
  // exercised explicitly here for clarity alongside the true-mode tests
  // below.
  ShardedCache<std::string, int> c(2, 1, /*use_pool=*/true, /*use_read_buffer=*/false,
                                    std::chrono::milliseconds(0));
  c.put("a", 1);
  c.put("b", 2);
  c.get("a");  // immediate reorder - no buffering in this mode
  c.put("c", 3);
  EXPECT_TRUE(c.get("a").has_value());
  EXPECT_FALSE(c.get("b").has_value());
}

TEST(ReadBuffer, OverflowForcesADrainWithCorrectLruOrder) {
  // Touch buffer capacity is 128 (see sharded_cache.hpp). Reading a key
  // more than that many times without an intervening put() forces an
  // internal drain purely from get()'s overflow path, not put()'s
  // pre-eviction drain - this test's only route to a correct answer is
  // that overflow-triggered drain actually working. use_read_buffer=true
  // explicitly here - this suite is what still exercises and verifies
  // that opt-in path even though it's off by default.
  ShardedCache<int, int> c(3, 1, true, /*use_read_buffer=*/true, std::chrono::milliseconds(0));
  c.put(1, 1);
  c.put(2, 2);
  c.put(3, 3);

  for (int i = 0; i < 200; ++i) {
    c.get(1);  // keep "1" hot well past the 128-touch buffer capacity
  }

  c.put(4, 4);  // capacity 3: should evict the actual LRU, not "1"
  EXPECT_TRUE(c.get(1).has_value()) << "key 1 was read 200x and should never be LRU";
  EXPECT_TRUE(c.get(4).has_value());
}

TEST(ReadBuffer, ConcurrentReadHeavyWorkloadStaysWithinCapacity) {
  // Correctness under the opt-in buffered path (use_read_buffer=true)
  // specifically, under a read-dominated (95% get) concurrent workload -
  // the shape buffered reads were designed for, even though it measured
  // as a net loss on this project's hardware (see README).
  constexpr int kCapacity = 100;
  ShardedCache<int, int> c(kCapacity, 4, true, /*use_read_buffer=*/true, std::chrono::milliseconds(0));
  for (int i = 0; i < kCapacity; ++i) c.put(i, i);

  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&c, t] {
      unsigned seed = static_cast<unsigned>(t) + 1;
      for (int i = 0; i < 30000; ++i) {
        seed = seed * 1103515245u + 12345u;
        int key = static_cast<int>((seed / 65536u) % kCapacity);
        if (i % 20 == 0) {
          c.put(key, key * 2);
        } else {
          c.get(key);
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_LE(c.size(), static_cast<std::size_t>(kCapacity));
}

TEST(Concurrency, ValuesAreNeverTornOrCorrupted) {
  // Each thread writes a value tagged with its own id; if a get() ever
  // observes a value not on the small fixed set of legal strings, that's
  // a torn read / corrupted node.
  ShardedCache<int, std::string> c(50, 4, true, false, std::chrono::milliseconds(0));
  std::atomic<bool> corruption{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::string tag = "writer-" + std::to_string(t);
      for (int i = 0; i < 20000; ++i) {
        int key = i % 30;
        c.put(key, tag);
        auto v = c.get(key);
        if (v.has_value() && v->rfind("writer-", 0) != 0) {
          corruption.store(true);
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_FALSE(corruption.load());
}
