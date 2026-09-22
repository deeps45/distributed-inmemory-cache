// Small manual demo of the L1/Redis-L2 tiered cache. Requires a Redis
// instance reachable at REDIS_HOST:REDIS_PORT (defaults localhost:6379 -
// see docker-compose.yml for a one-command local Redis).
#include <cstdlib>
#include <iostream>

#include "cache/redis_client.hpp"
#include "cache/sharded_cache.hpp"
#include "cache/tiered_cache.hpp"

int main() {
  const char* host_env = std::getenv("REDIS_HOST");
  const char* port_env = std::getenv("REDIS_PORT");
  std::string host = host_env != nullptr ? host_env : "127.0.0.1";
  int port = port_env != nullptr ? std::atoi(port_env) : 6379;

  cache::RedisClient redis(host, port);
  if (!redis.connected()) {
    std::cerr << "Could not connect to Redis at " << host << ":" << port << "\n";
    std::cerr << "Start one with: docker compose up -d redis\n";
    return 1;
  }

  cache::ShardedCache<std::string, std::string> l1(/*capacity=*/1000, /*shards=*/8);
  cache::TieredCache tiered(l1, redis);

  std::cout << "Connected to Redis at " << host << ":" << port << "\n\n";

  tiered.put("user:42:name", "Ada Lovelace", std::chrono::seconds(30));
  std::cout << "put user:42:name = Ada Lovelace (ttl 30s)\n";

  auto v1 = tiered.get("user:42:name");  // L2 hit (nothing in L1 yet on a fresh process)
  std::cout << "get #1 -> " << (v1 ? *v1 : "(miss)") << "\n";

  auto v2 = tiered.get("user:42:name");  // L1 hit now - backfilled by the read above
  std::cout << "get #2 -> " << (v2 ? *v2 : "(miss)") << " (should now be an L1 hit)\n";

  auto stats = tiered.stats();
  std::cout << "\nstats: l1_hits=" << stats.l1_hits << " l1_misses=" << stats.l1_misses
            << " l2_hits=" << stats.l2_hits << " l2_misses=" << stats.l2_misses << "\n";
  std::cout << "\nSee benchmarks/bench_tiered.cpp for the measured latency gap between an L1 "
               "hit and this L2 round trip.\n";
  return 0;
}
