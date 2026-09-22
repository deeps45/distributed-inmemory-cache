#include "cache/redis_client.hpp"

#include <hiredis/hiredis.h>

#include <cstring>

namespace cache {

RedisClient::RedisClient(const std::string& host, int port, std::chrono::milliseconds connect_timeout) {
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(connect_timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((connect_timeout.count() % 1000) * 1000);
  redisContext* ctx = redisConnectWithTimeout(host.c_str(), port, tv);
  if (ctx == nullptr || ctx->err) {
    if (ctx != nullptr) redisFree(ctx);
    ctx_ = nullptr;
    return;
  }
  ctx_ = ctx;
}

RedisClient::~RedisClient() {
  if (ctx_ != nullptr) redisFree(ctx_);
}

std::optional<std::string> RedisClient::get(const std::string& key) {
  if (ctx_ == nullptr) return std::nullopt;
  auto* reply = static_cast<redisReply*>(redisCommand(ctx_, "GET %s", key.c_str()));
  if (reply == nullptr) return std::nullopt;
  std::optional<std::string> result;
  if (reply->type == REDIS_REPLY_STRING) {
    result = std::string(reply->str, reply->len);
  }
  freeReplyObject(reply);
  return result;
}

bool RedisClient::set(const std::string& key, const std::string& value,
                       std::optional<std::chrono::milliseconds> ttl) {
  if (ctx_ == nullptr) return false;
  redisReply* reply;
  if (ttl.has_value()) {
    reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %s PX %lld", key.c_str(), value.c_str(),
                     static_cast<long long>(ttl->count())));
  } else {
    reply = static_cast<redisReply*>(redisCommand(ctx_, "SET %s %s", key.c_str(), value.c_str()));
  }
  if (reply == nullptr) return false;
  bool ok = reply->type == REDIS_REPLY_STATUS && std::strcmp(reply->str, "OK") == 0;
  freeReplyObject(reply);
  return ok;
}

bool RedisClient::del(const std::string& key) {
  if (ctx_ == nullptr) return false;
  auto* reply = static_cast<redisReply*>(redisCommand(ctx_, "DEL %s", key.c_str()));
  if (reply == nullptr) return false;
  bool ok = reply->type == REDIS_REPLY_INTEGER;
  freeReplyObject(reply);
  return ok;
}

}  // namespace cache
