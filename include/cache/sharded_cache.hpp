#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
// `shard_count` independent partitions.
//
// Design notes (see README for the full research writeup):
//
// - Sharding, not a single global lock: each key hashes to exactly one
//   shard, and each shard has its own lock, its own hash map, its own
//   intrusive LRU list, and (optionally) its own object pool. Threads
//   touching different shards never contend. This is the same idea behind
//   Java's ConcurrentHashMap segment locking and Memcached's per-bucket
//   locking - shard_count=1 recovers the naive single-lock baseline
//   exactly, which is what makes the sharded-vs-baseline benchmark in
//   benchmarks/bench_cache.cpp a fair, apples-to-apples comparison of one
//   implementation, not two different ones.
//
// - Buffered reads (use_read_buffer, default false - see below): implemented
//   on the same premise Caffeine (the standard high-performance JVM cache)
//   uses - a get() is not read-only at the data-structure level in a plain
//   LRU cache, since a hit moves that entry to the front of the list, so a
//   naive implementation needs exclusive access on every hit, which is
//   most of a working cache's traffic. This mode instead takes a *shared*
//   lock to read the map (so concurrent hits on different keys don't
//   block each other) and records the touch by appending the key to a
//   small per-shard buffer guarded by its own separate mutex, replayed
//   onto the real LRU list in batches later, off the hot path.
//
//   It measures SLOWER than the plain exclusive-lock design on this
//   project's benchmarks, at every thread count tested, including
//   single-threaded with zero contention (see README - "Buffered reads:
//   measured, and rejected"). Root cause, isolated with a standalone
//   microbenchmark: on this platform's libc++, an uncontended
//   std::shared_mutex costs roughly 2x an uncontended std::mutex for the
//   same critical section (~18ns vs ~8ns here) - this mode pays that tax
//   on every get() *and* adds a second lock acquisition (the touch-buffer
//   mutex) on the hit path, and a hashmap lookup is far too short a
//   critical section to amortize either cost, let alone both. Caffeine's
//   real implementation uses a genuinely lock-free ring buffer, not a
//   second mutex, and runs in a context where the comparison looks
//   different; a mutex-guarded buffer was a reasonable-sounding
//   simplification that the data disproved. Left in, off by default,
//   because it's a real, TSan-verified-correct implementation of a real
//   technique that happens not to pay off *here* - measuring and
//   reporting that honestly is the point, not deleting the evidence. See
//   tests/stress_test.cpp and the ReadBuffer.* cases in
//   tests/unit_tests.cpp for its correctness coverage, and
//   benchmarks/bench_cache.cpp for the A/B that found this.
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
               bool use_read_buffer = false,
               std::chrono::milliseconds reap_interval = std::chrono::milliseconds(200))
      : shard_count_(shard_count == 0 ? 1 : shard_count) {
    std::size_t per_shard = std::max<std::size_t>(1, total_capacity / shard_count_);
    shards_.reserve(shard_count_);
    for (std::size_t i = 0; i < shard_count_; ++i) {
      shards_.push_back(std::make_unique<Shard>(per_shard, use_pool, use_read_buffer));
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
    Shard(std::size_t capacity, bool use_pool, bool use_read_buffer)
        : capacity_(capacity), use_pool_(use_pool), use_read_buffer_(use_read_buffer) {
      touch_buffer_.reserve(kTouchBufferCapacity);
    }

    ~Shard() {
      std::lock_guard<std::shared_mutex> lock(mutex_);
      Node* node = head_;
      while (node != nullptr) {
        Node* next = node->next;
        destroy_node(node);
        node = next;
      }
    }

    std::optional<V> get(const K& key) {
      return use_read_buffer_ ? get_buffered(key) : get_exclusive(key);
    }

    void put(const K& key, V value, std::optional<std::chrono::milliseconds> ttl) {
      auto expires_at = ttl.has_value() ? std::optional(std::chrono::steady_clock::now() + *ttl)
                                         : std::nullopt;
      std::vector<K> pending = use_read_buffer_ ? take_pending_touches() : std::vector<K>{};

      std::lock_guard<std::shared_mutex> lock(mutex_);
      if (!pending.empty()) apply_touches_locked(pending);

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
      std::lock_guard<std::shared_mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it == map_.end()) return false;
      Node* node = it->second;
      unlink(node);
      map_.erase(it);
      destroy_node(node);
      return true;
    }

    std::size_t size() const {
      std::shared_lock<std::shared_mutex> lock(mutex_);
      return map_.size();
    }

    Stats stats() const {
      return Stats{hits_.load(std::memory_order_relaxed), misses_.load(std::memory_order_relaxed),
                   evictions_.load(std::memory_order_relaxed),
                   expirations_.load(std::memory_order_relaxed)};
    }

    // Called periodically by the owning cache's reaper thread. Walks the
    // whole shard once, unlinking anything past its TTL. O(shard size) per
    // sweep - fine at the scale this project benchmarks at, and simpler to
    // reason about (and verify race-free) than a secondary min-heap
    // ordered by expiry, which would be the next step for a cache with
    // many long-lived, rarely-touched keys mixed with short-TTL ones.
    void sweep_expired() {
      std::lock_guard<std::shared_mutex> lock(mutex_);
      Node* node = head_;
      while (node != nullptr) {
        Node* next = node->next;
        if (is_expired(node)) {
          unlink(node);
          map_.erase(node->key);
          destroy_node(node);
          expirations_.fetch_add(1, std::memory_order_relaxed);
        }
        node = next;
      }
    }

   private:
    std::optional<V> get_exclusive(const K& key) {
      std::lock_guard<std::shared_mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it == map_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
      }
      Node* node = it->second;
      if (is_expired(node)) {
        unlink(node);
        map_.erase(it);
        destroy_node(node);
        misses_.fetch_add(1, std::memory_order_relaxed);
        expirations_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
      }
      touch(node);
      hits_.fetch_add(1, std::memory_order_relaxed);
      return node->value;
    }

    std::optional<V> get_buffered(const K& key) {
      {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end() && !is_expired(it->second)) {
          V value_copy = it->second->value;
          hits_.fetch_add(1, std::memory_order_relaxed);
          // Must release the shared lock before record_touch(): if the
          // touch buffer happens to be full, that call takes this same
          // shard's lock exclusively to drain it, which would deadlock
          // against a shared lock this thread still held.
          lock.unlock();
          record_touch(key);
          return value_copy;
        }
      }

      // Miss or expired - both are rare relative to hits in a working
      // cache, so paying for exclusive access here is fine.
      std::lock_guard<std::shared_mutex> lock(mutex_);
      auto it = map_.find(key);
      if (it != map_.end()) {
        if (!is_expired(it->second)) {
          // A concurrent put() raced in between our shared-lock miss above
          // and acquiring this exclusive lock. Honor it as a hit rather
          // than report an incorrect miss for a key that's actually
          // present right now.
          V value_copy = it->second->value;
          touch(it->second);
          hits_.fetch_add(1, std::memory_order_relaxed);
          return value_copy;
        }
        Node* node = it->second;
        unlink(node);
        map_.erase(it);
        destroy_node(node);
        expirations_.fetch_add(1, std::memory_order_relaxed);
      }
      misses_.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    // Appends `key` to the pending-touch buffer under a small dedicated
    // mutex - separate from, and held far more briefly than, the shard's
    // main lock. Triggers a drain once the buffer fills.
    void record_touch(const K& key) {
      bool need_drain = false;
      {
        std::lock_guard<std::mutex> lg(touch_buffer_mutex_);
        touch_buffer_.push_back(key);
        need_drain = touch_buffer_.size() >= kTouchBufferCapacity;
      }
      if (need_drain) drain_touches_external();
    }

    std::vector<K> take_pending_touches() {
      std::lock_guard<std::mutex> lg(touch_buffer_mutex_);
      std::vector<K> local;
      local.swap(touch_buffer_);
      return local;
    }

    // Caller must already hold mutex_ exclusively.
    void apply_touches_locked(const std::vector<K>& touches) {
      for (const auto& k : touches) {
        auto it = map_.find(k);
        // If the key isn't present any more, it was evicted or erased
        // since the touch was recorded - nothing to do. This is exactly
        // the dangling-pointer hazard this design avoids by storing keys,
        // not node pointers: a stale pointer here would be a use-after-free.
        if (it != map_.end()) touch(it->second);
      }
    }

    void drain_touches_external() {
      auto local = take_pending_touches();
      if (local.empty()) return;
      std::lock_guard<std::shared_mutex> lock(mutex_);
      apply_touches_locked(local);
    }

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
      evictions_.fetch_add(1, std::memory_order_relaxed);
    }

    Node* construct_node(const K& key, V value, std::optional<std::chrono::steady_clock::time_point> exp) {
      if (use_pool_) return pool_.construct(key, std::move(value), exp);
      return new Node(key, std::move(value), exp);
    }

    void destroy_node(Node* node) {
      if (use_pool_) pool_.destroy(node); else delete node;
    }

    static constexpr std::size_t kTouchBufferCapacity = 128;

    mutable std::shared_mutex mutex_;
    std::size_t capacity_;
    bool use_pool_;
    bool use_read_buffer_;
    std::unordered_map<K, Node*, Hash> map_;
    Node* head_ = nullptr;  // most recently used
    Node* tail_ = nullptr;  // least recently used
    detail::ObjectPool<Node> pool_;

    std::mutex touch_buffer_mutex_;
    std::vector<K> touch_buffer_;

    std::atomic<std::size_t> hits_{0};
    std::atomic<std::size_t> misses_{0};
    std::atomic<std::size_t> evictions_{0};
    std::atomic<std::size_t> expirations_{0};
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
