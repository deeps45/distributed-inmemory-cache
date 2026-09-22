#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cache/object_pool.hpp"

namespace cache {

struct Stats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t evictions = 0;   // LRU-evicted to make room
  std::size_t expirations = 0; // removed because TTL passed (lazy or reaper)

  Stats& operator+=(const Stats& other) {
    hits += other.hits;
    misses += other.misses;
    evictions += other.evictions;
    expirations += other.expirations;
    return *this;
  }
};

// A concurrent, TTL- and LRU-aware key-value cache, sharded across
// `shard_count` independent mutex-guarded partitions.
//
// Design notes (see README for the full research writeup):
//
// - Sharding, not a single global lock: each key hashes to exactly one
//   shard, and each shard has its own mutex, its own hash map, its own
//   intrusive LRU list, and (optionally) its own object pool. Threads
//   touching different shards never contend. This is the same idea behind
//   Java's ConcurrentHashMap segment locking and Memcached's per-bucket
//   locking - shard_count=1 recovers the naive single-lock baseline
//   exactly, which is what makes the sharded-vs-baseline benchmark in
//   benchmarks/bench_cache.cpp a fair, apples-to-apples comparison of one
//   implementation, not two different ones.
//
// - Exclusive (not shared/reader-writer) locking per shard, on purpose:
//   a cache get() is not read-only at the data-structure level - a hit
//   moves that entry to the front of the LRU list, which mutates shared
//   state. A std::shared_mutex would only avoid contention between
//   *misses*, while every hit still needs exclusive access, which is most
//   of the traffic in a well-tuned cache. High-performance caches that
//   truly parallelize reads (e.g. Caffeine, on the JVM) do it by buffering
//   read events into a lock-free ring buffer and replaying them onto the
//   LRU structure in batches later, off the hot path - noted here as
//   future work rather than implemented, to keep this shard's logic
//   simple enough to reason about (and to verify with ThreadSanitizer -
//   see tests/stress_test.cpp).
//
// - TTL expiration is both lazy (checked on get()) and active (a
//   background thread periodically sweeps each shard) - the same hybrid
//   strategy Redis itself documents using, for the same reason: lazy-only
//   expiration never reclaims memory for keys nobody asks for again, and
//   active-only expiration on a timer alone would leave a stale value
//   visible to a get() that lands between sweeps.
template <typename K, typename V, typename Hash = std::hash<K>>
class ShardedCache {
 public:
  ShardedCache(std::size_t total_capacity, std::size_t shard_count, bool use_pool = true,
               std::chrono::milliseconds reap_interval = std::chrono::milliseconds(200))
      : shard_count_(shard_count == 0 ? 1 : shard_count) {
    std::size_t per_shard = std::max<std::size_t>(1, total_capacity / shard_count_);
    shards_.reserve(shard_count_);
    for (std::size_t i = 0; i < shard_count_; ++i) {
      shards_.push_back(std::make_unique<Shard>(per_shard, use_pool));
    }
    if (reap_interval.count() > 0) {
      reaper_ = std::jthread([this, reap_interval](std::stop_token stoken) {
        reaper_loop(stoken, reap_interval);
      });
    }
  }

  ~ShardedCache() = default;

  ShardedCache(const ShardedCache&) = delete;
  ShardedCache& operator=(const ShardedCache&) = delete;

  std::optional<V> get(const K& key) {
    Shard& shard = shard_for(key);
    return shard.get(key);
  }

  void put(const K& key, V value, std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
    Shard& shard = shard_for(key);
    shard.put(key, std::move(value), ttl);
  }

  bool erase(const K& key) {
    Shard& shard = shard_for(key);
    return shard.erase(key);
  }

  std::size_t size() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) total += shard->size();
    return total;
  }

  Stats stats() const {
    Stats total;
    for (const auto& shard : shards_) total += shard->stats();
    return total;
  }

  std::size_t shard_count() const { return shard_count_; }

 private:
  struct Node {
    K key;
    V value;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
    Node* prev = nullptr;
    Node* next = nullptr;

    Node(K k, V v, std::optional<std::chrono::steady_clock::time_point> exp)
        : key(std::move(k)), value(std::move(v)), expires_at(exp) {}
  };

  class Shard {
   public:
    Shard(std::size_t capacity, bool use_pool) : capacity_(capacity), use_pool_(use_pool) {}

    ~Shard() {
      std::lock_guard<std::mutex> lock(mutex_);
      Node* node = head_;
      while (node != nullptr) {
        Node* next = node->next;
        destroy_node(node);
        node = next;
      }
    }

    std::optional<V> get(const K& key) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it == map_.end()) {
        ++stats_.misses;
        return std::nullopt;
      }
      Node* node = it->second;
      if (is_expired(node)) {
        unlink(node);
        map_.erase(it);
        destroy_node(node);
        ++stats_.misses;
        ++stats_.expirations;
        return std::nullopt;
      }
      touch(node);
      ++stats_.hits;
      return node->value;
    }

    void put(const K& key, V value, std::optional<std::chrono::milliseconds> ttl) {
      auto expires_at = ttl.has_value() ? std::optional(std::chrono::steady_clock::now() + *ttl)
                                         : std::nullopt;
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it != map_.end()) {
        Node* node = it->second;
        node->value = std::move(value);
        node->expires_at = expires_at;
        touch(node);
        return;
      }
      if (map_.size() >= capacity_) {
        evict_lru();
      }
      Node* node = construct_node(key, std::move(value), expires_at);
      map_.emplace(key, node);
      push_front(node);
    }

    bool erase(const K& key) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it == map_.end()) return false;
      Node* node = it->second;
      unlink(node);
      map_.erase(it);
      destroy_node(node);
      return true;
    }

    std::size_t size() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return map_.size();
    }

    Stats stats() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return stats_;
    }

    // Called periodically by the owning cache's reaper thread. Walks the
    // whole shard once, unlinking anything past its TTL. O(shard size) per
    // sweep - fine at the scale this project benchmarks at, and simpler to
    // reason about (and verify race-free) than a secondary min-heap
    // ordered by expiry, which would be the next step for a cache with
    // many long-lived, rarely-touched keys mixed with short-TTL ones.
    void sweep_expired() {
      std::lock_guard<std::mutex> lock(mutex_);
      Node* node = head_;
      while (node != nullptr) {
        Node* next = node->next;
        if (is_expired(node)) {
          unlink(node);
          map_.erase(node->key);
          destroy_node(node);
          ++stats_.expirations;
        }
        node = next;
      }
    }

   private:
    bool is_expired(const Node* node) const {
      return node->expires_at.has_value() && *node->expires_at <= std::chrono::steady_clock::now();
    }

    // Unlinks from the intrusive list only - caller handles the map.
    void unlink(Node* node) {
      if (node->prev != nullptr) node->prev->next = node->next; else head_ = node->next;
      if (node->next != nullptr) node->next->prev = node->prev; else tail_ = node->prev;
      node->prev = node->next = nullptr;
    }

    void push_front(Node* node) {
      node->prev = nullptr;
      node->next = head_;
      if (head_ != nullptr) head_->prev = node;
      head_ = node;
      if (tail_ == nullptr) tail_ = node;
    }

    void touch(Node* node) {
      if (head_ == node) return;
      unlink(node);
      push_front(node);
    }

    void evict_lru() {
      if (tail_ == nullptr) return;
      Node* victim = tail_;
      unlink(victim);
      map_.erase(victim->key);
      destroy_node(victim);
      ++stats_.evictions;
    }

    Node* construct_node(const K& key, V value, std::optional<std::chrono::steady_clock::time_point> exp) {
      if (use_pool_) return pool_.construct(key, std::move(value), exp);
      return new Node(key, std::move(value), exp);
    }

    void destroy_node(Node* node) {
      if (use_pool_) pool_.destroy(node); else delete node;
    }

    mutable std::mutex mutex_;
    std::size_t capacity_;
    bool use_pool_;
    std::unordered_map<K, Node*, Hash> map_;
    Node* head_ = nullptr;  // most recently used
    Node* tail_ = nullptr;  // least recently used
    detail::ObjectPool<Node> pool_;
    Stats stats_;
  };

  Shard& shard_for(const K& key) { return *shards_[hasher_(key) % shard_count_]; }

  void reaper_loop(std::stop_token stoken, std::chrono::milliseconds interval) {
    while (!stoken.stop_requested()) {
      for (auto& shard : shards_) {
        if (stoken.stop_requested()) return;
        shard->sweep_expired();
      }
      std::this_thread::sleep_for(interval);
    }
  }

  std::size_t shard_count_;
  Hash hasher_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::jthread reaper_;  // stops and joins automatically on destruction
};

}  // namespace cache
