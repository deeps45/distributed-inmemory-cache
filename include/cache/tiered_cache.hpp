#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "cache/redis_client.hpp"
#include "cache/sharded_cache.hpp"

namespace cache {

// The actual value proposition of this project, made concrete: an
// in-process L1 (ShardedCache) in front of Redis as L2. Redis alone is
// already fast, but every GET still costs a network (or at minimum a
// loopback + syscall + serialization) round trip; an L1 hit costs a
// hashmap lookup and a few pointer swaps under a mutex already held by
// nobody else 15 out of 16 times (with 16 shards). See
// benchmarks/bench_tiered.cpp for the measured gap between the two on
// this machine - it's the whole reason a project pairing "C++,
// multithreading" with "Redis" makes sense as more than a name-drop.
struct TieredStats {
  std::size_t l1_hits = 0;
  std::size_t l1_misses = 0;
  std::size_t l2_hits = 0;
  std::size_t l2_misses = 0;
};

class TieredCache {
 public:
  TieredCache(ShardedCache<std::string, std::string>& l1, RedisClient& l2) : l1_(l1), l2_(l2) {}

  std::optional<std::string> get(const std::string& key) {
    if (auto v = l1_.get(key)) {
      l1_hits_.fetch_add(1, std::memory_order_relaxed);
      return v;
    }
    l1_misses_.fetch_add(1, std::memory_order_relaxed);

    auto v = l2_.get(key);
    if (v.has_value()) {
      l2_hits_.fetch_add(1, std::memory_order_relaxed);
      l1_.put(key, *v);  // backfill L1 so the next read is a local hit
    } else {
      l2_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return v;
  }

  // Write-through: L2 (Redis) is the durable source of truth, L1 is
  // populated eagerly so a read-after-write doesn't have to round-trip.
  void put(const std::string& key, const std::string& value,
           std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
    l2_.set(key, value, ttl);
    l1_.put(key, value, ttl);
  }

  void erase(const std::string& key) {
    l2_.del(key);
    l1_.erase(key);
  }

  TieredStats stats() const {
    return {l1_hits_.load(), l1_misses_.load(), l2_hits_.load(), l2_misses_.load()};
  }

 private:
  ShardedCache<std::string, std::string>& l1_;
  RedisClient& l2_;
  std::atomic<std::size_t> l1_hits_{0};
  std::atomic<std::size_t> l1_misses_{0};
  std::atomic<std::size_t> l2_hits_{0};
  std::atomic<std::size_t> l2_misses_{0};
};

}  // namespace cache
