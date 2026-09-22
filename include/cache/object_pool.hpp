#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace cache::detail {

// Fixed-size free-list allocator for a single type T. Avoids a malloc/free
// (or new/delete) round trip on every cache node create/destroy, which
// matters here because every put() and every LRU eviction allocates or
// frees exactly one node.
//
// Not thread-safe by itself - each Shard owns one pool and only ever
// touches it while already holding that shard's mutex, so no extra
// synchronization is added here. A shared pool across shards would defeat
// the point of sharding (it would become the new contention point).
template <typename T>
class ObjectPool {
  union Slot {
    T value;
    Slot* next_free;
    Slot() {}
    ~Slot() {}
  };

 public:
  explicit ObjectPool(std::size_t chunk_size = 256) : chunk_size_(chunk_size) {}

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  template <typename... Args>
  T* construct(Args&&... args) {
    if (free_head_ == nullptr) {
      allocate_chunk();
    }
    Slot* slot = free_head_;
    free_head_ = slot->next_free;
    return new (&slot->value) T(std::forward<Args>(args)...);
  }

  void destroy(T* obj) {
    obj->~T();
    Slot* slot = reinterpret_cast<Slot*>(obj);
    slot->next_free = free_head_;
    free_head_ = slot;
  }

  std::size_t chunks_allocated() const { return chunks_.size(); }

 private:
  void allocate_chunk() {
    auto chunk = std::make_unique<Slot[]>(chunk_size_);
    for (std::size_t i = 0; i + 1 < chunk_size_; ++i) {
      chunk[i].next_free = &chunk[i + 1];
    }
    chunk[chunk_size_ - 1].next_free = free_head_;
    free_head_ = &chunk[0];
    chunks_.push_back(std::move(chunk));
  }

  std::size_t chunk_size_;
  Slot* free_head_ = nullptr;
  std::vector<std::unique_ptr<Slot[]>> chunks_;
};

}  // namespace cache::detail
