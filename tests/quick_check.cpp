#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache/sharded_cache.hpp"

int main() {
  cache::ShardedCache<std::string, std::string> c(/*capacity=*/3, /*shards=*/1, /*use_pool=*/true,
                                                    std::chrono::milliseconds(0));

  c.put("a", "1");
  c.put("b", "2");
  c.put("c", "3");
  assert(c.get("a").value() == "1");

  // "a" now MRU; "b" is LRU. Inserting "d" should evict "b".
  c.put("d", "4");
  assert(!c.get("b").has_value());
  assert(c.get("a").has_value());
  assert(c.get("c").has_value());
  assert(c.get("d").has_value());

  // TTL
  cache::ShardedCache<std::string, std::string> ttl_cache(10, 1, true, std::chrono::milliseconds(0));
  ttl_cache.put("x", "1", std::chrono::milliseconds(10));
  assert(ttl_cache.get("x").has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  assert(!ttl_cache.get("x").has_value());

  // Basic concurrent smoke test
  cache::ShardedCache<int, int> conc(1000, 8, true, std::chrono::milliseconds(50));
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&conc, t] {
      for (int i = 0; i < 10000; ++i) {
        int key = (t * 10000 + i) % 500;
        conc.put(key, key);
        conc.get(key);
      }
    });
  }
  for (auto& th : threads) th.join();
  assert(conc.size() <= 1000);

  std::cout << "all quick checks passed\n";
  return 0;
}
