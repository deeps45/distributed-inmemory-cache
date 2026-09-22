#pragma once

#include <chrono>
#include <optional>
#include <string>

// Forward-declared to avoid leaking hiredis's C header into every
// translation unit that includes this file - only redis_client.cpp needs
// it. Deliberately minimal: just enough (GET/SETEX/DEL) to back the L2
// tier in tiered_cache.hpp, not a general Redis client.
struct redisContext;

namespace cache {

class RedisClient {
 public:
  RedisClient(const std::string& host, int port, std::chrono::milliseconds connect_timeout =
                                                       std::chrono::milliseconds(1000));
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  bool connected() const { return ctx_ != nullptr; }

  std::optional<std::string> get(const std::string& key);
  bool set(const std::string& key, const std::string& value,
            std::optional<std::chrono::milliseconds> ttl = std::nullopt);
  bool del(const std::string& key);

 private:
  redisContext* ctx_ = nullptr;
};

}  // namespace cache
