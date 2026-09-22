// Measures the actual gap an L1 (in-process) cache closes in front of
// Redis: an L1 hit vs. a direct Redis GET (even over loopback, which is
// the best case for Redis - a real network hop would widen this gap
// further). Requires a reachable Redis - see docker-compose.yml.
#include <chrono>
#include <cstdlib>
#include <string>

#include <benchmark/benchmark.h>

#include "cache/redis_client.hpp"
#include "cache/sharded_cache.hpp"
#include "cache/tiered_cache.hpp"

namespace {

std::string RedisHost() {
  const char* h = std::getenv("REDIS_HOST");
  return h != nullptr ? h : "127.0.0.1";
}

int RedisPort() {
  const char* p = std::getenv("REDIS_PORT");
  return p != nullptr ? std::atoi(p) : 6379;
}

void BM_L1_Hit(benchmark::State& state) {
  cache::ShardedCache<std::string, std::string> l1(1000, 8);
  l1.put("bench-key", "bench-value");
  for (auto _ : state) {
    benchmark::DoNotOptimize(l1.get("bench-key"));
  }
}
BENCHMARK(BM_L1_Hit);

void BM_Redis_Direct_Get(benchmark::State& state) {
  cache::RedisClient redis(RedisHost(), RedisPort());
  if (!redis.connected()) {
    state.SkipWithError("Redis not reachable - start it with: docker compose up -d redis");
    return;
  }
  redis.set("bench-key", "bench-value");
  for (auto _ : state) {
    benchmark::DoNotOptimize(redis.get("bench-key"));
  }
}
BENCHMARK(BM_Redis_Direct_Get);

void BM_TieredCache_L1HitAfterWarm(benchmark::State& state) {
  cache::ShardedCache<std::string, std::string> l1(1000, 8);
  cache::RedisClient redis(RedisHost(), RedisPort());
  if (!redis.connected()) {
    state.SkipWithError("Redis not reachable - start it with: docker compose up -d redis");
    return;
  }
  cache::TieredCache tiered(l1, redis);
  tiered.put("bench-key", "bench-value");
  tiered.get("bench-key");  // first read backfills L1; everything after is a local hit
  for (auto _ : state) {
    benchmark::DoNotOptimize(tiered.get("bench-key"));
  }
}
BENCHMARK(BM_TieredCache_L1HitAfterWarm);

}  // namespace
