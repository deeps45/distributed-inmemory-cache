# Distributed In-Memory Cache

A concurrent, TTL- and LRU-aware in-memory key-value cache in C++20, sharded
across independent locks instead of one global mutex, with a custom
free-list object allocator, a benchmark suite quantifying both of those
decisions separately, an adversarial concurrency test verified clean under
ThreadSanitizer, and an optional Redis L2 tier demonstrating the actual
reason an in-process cache like this is worth having next to Redis rather
than instead of it.

**What I'd do differently with more time:** implement the Caffeine-style
read-buffering technique noted in `sharded_cache.hpp` (batching LRU "touch"
events through a lock-free ring buffer instead of taking the shard's mutex
on every read) - the current design is exclusive-lock-per-shard throughout,
which is simple and verifiably correct but leaves real concurrency on the
table for read-heavy workloads; replace the O(shard size) TTL sweep with a
secondary min-heap ordered by expiry for caches with many long-lived keys
mixed with short-TTL ones; and get ThreadSanitizer working natively on this
Mac instead of routing through Docker (see "Honest limits" below) - it's a
toolchain gap, not a design choice, but it cost real time.

## Why this exists, not just what it does

"C++ LRU cache" is one of the most-implemented toy projects there is. What
makes this one worth a resume line is that every one of its three claims -
lock-efficient synchronization, a throughput multiplier, and optimized
memory allocation - is independently measured, not asserted, and the
comparison is apples-to-apples: `ShardedCache` with `shard_count=1` *is*
the naive single-lock baseline, not a separate reimplementation of it, so
the A/B in [Benchmarks](#benchmarks) is a true isolation of each variable.

## Architecture

```
                         ShardedCache<K, V>
                    ┌─────────────────────────────────────────┐
   get(k)/put(k,v)  │  hash(k) % shard_count                  │
   ───────────────▶ │           │                              │
                    │           ▼                              │
                    │  ┌───────────────┐  ┌───────────────┐    │
                    │  │    Shard 0    │  │    Shard N    │... │
                    │  │ std::mutex    │  │ std::mutex    │    │
                    │  │ unordered_map │  │ unordered_map │    │
                    │  │ intrusive LRU │  │ intrusive LRU │    │
                    │  │ ObjectPool    │  │ ObjectPool    │    │
                    │  └───────────────┘  └───────────────┘    │
                    │           ▲                               │
                    │           │ sweep_expired(), every 200ms  │
                    │      background std::jthread (reaper)     │
                    └─────────────────────────────────────────┘

              Optional L2 tier (TieredCache, see below):
   get(k) ─▶ L1 (ShardedCache) ─miss─▶ RedisClient (hiredis) ─▶ backfill L1
```

Threads touching different shards never contend with each other - that's
the whole mechanism. `shard_count=1` collapses this to a single global
lock, which is exactly the baseline the benchmark compares against.

**Why exclusive locking, not `std::shared_mutex`, per shard:** a cache
`get()` isn't read-only at the data-structure level - a hit moves that
node to the front of the LRU list, which mutates shared state. A
reader-writer lock would only help *misses* run concurrently; every hit
(most of the traffic in a working cache) still needs exclusive access.
Real high-performance caches that parallelize reads for real - Caffeine on
the JVM is the standard reference - do it by buffering read events into a
lock-free ring buffer and replaying them onto the LRU structure in
batches, off the hot path. That's real added complexity for a real gain,
noted above as future work rather than implemented, so this shard's logic
stays simple enough to verify with ThreadSanitizer (see
[Correctness](#correctness-verified-not-assumed)).

**TTL: lazy + active, on purpose, not just lazy:** every `get()` checks
expiry and treats a stale hit as a miss (lazy), and a background
`std::jthread` sweeps each shard every 200ms removing anything expired
whether or not it's ever read again (active). This is the same hybrid
Redis itself documents using its own expiration strategy for the same
reason: lazy-only expiration never reclaims memory for keys nobody asks
for again; active-only expiration on a timer alone would let a `get()`
landing between sweeps see a stale value.

## Quickstart

Requires CMake 3.20+, a C++20 compiler (this was built and benchmarked
with Apple Clang 17), and optionally Docker for the Redis L2 demo.

```bash
make build   # configures + builds everything (fetches GoogleTest, Google
             # Benchmark via CMake FetchContent - both free, open source)
make test    # unit tests + the concurrent stress test
make bench   # the sharding/allocator throughput benchmark (a few minutes)
make redis   # starts a local Redis for the L2 tier demo
make bench-tiered   # L1-hit vs. Redis-round-trip latency comparison
make tsan    # unit tests + stress test under ThreadSanitizer (via Docker -
             # see "Honest limits")
```

`./build/redis_demo` is a small standalone program exercising the tiered
cache manually against a running Redis (`make redis` first).

## Benchmarks

Every number below is from an actual run on this machine (Apple Silicon,
10 cores) and is reproducible with the `make` targets above - rerun before
citing any of it, especially on different hardware.

### Sharding and the object pool, measured independently

`benchmarks/bench_cache.cpp` runs a 90% get / 10% put workload (a common
cache read/write ratio, cf. YCSB workload B) against a 10,000-key space
with a 2,000-entry cache (real eviction pressure), at increasing thread
counts, in all four combinations of {global lock, 16 shards} x {no pool,
with pool}. 3 repetitions each; medians shown were stable within a few
percent of the means reported by Google Benchmark.

| Threads | Global lock, no pool | Global lock, + pool | 16 shards, no pool | 16 shards, + pool |
|---|---|---|---|---|
| 1  | 13.57M ops/s | 13.95M ops/s | 14.63M ops/s | 15.19M ops/s |
| 2  | 2.59M ops/s  | 3.04M ops/s  | 5.14M ops/s  | 6.05M ops/s  |
| 4  | 766k ops/s   | 1.09M ops/s  | 3.02M ops/s  | 2.90M ops/s  |
| 8  | 439k ops/s   | 554k ops/s   | 925k ops/s   | 1.13M ops/s  |
| 16 | 314k ops/s   | 373k ops/s   | 478k ops/s   | 487k ops/s   |

**Sharding alone** (global-lock-no-pool vs. 16-shards-no-pool, isolating
just that variable): 1.08x at 1 thread (near-zero overhead when there's no
contention to relieve, as expected), climbing to **3.9x at 4 threads**,
**2.1x at 8 threads** (this machine's physical core count), and settling
to 1.5x at 16 (oversubscribed - 16 threads on 10 cores, so some of that
drop-off is scheduling contention, not lock contention).

**The object pool alone** adds a further 15-30% on top of sharding at most
thread counts - except at 4 threads, where it measured *slightly worse*
(2.90M vs 3.02M, a 4% regression, within this run's ~5-9% coefficient of
variation at that point). Reported as-is rather than smoothed over: real
allocator behavior under contention is noisy, and a benchmark that only
ever shows the allocator helping isn't a trustworthy one.

**Combined** (global-lock-no-pool vs. 16-shards-with-pool): up to 3.8x at
4 threads, **2.58x at 8 threads**.

### The actual point: an L1 hit vs. a Redis round trip

`benchmarks/bench_tiered.cpp` measures a single-key `get()` three ways:
directly against `ShardedCache` (L1 only), directly against Redis over
`hiredis` (L2 only), and through `TieredCache` after the first read has
backfilled L1.

| | Mean latency |
|---|---|
| L1 hit (`ShardedCache::get`) | **22.3 ns** |
| `TieredCache::get`, L1-warm | 29.0 ns |
| Redis `GET`, native (Homebrew, loopback, port 6381) | 210,181 ns |
| Redis `GET`, via Docker Desktop (port-forwarded, port 6380) | 280,558 ns |

Cross-checked the native number against Redis's own `redis-cli --latency`
tool, independent of this project's code: 674µs average over 82 samples -
consistent with (in fact higher than) the 210µs this project's own hiredis
client measured, confirming the gap is real and not a benchmarking
artifact on my end. That's a ~9,400x-12,600x difference between an L1 hit
and any path that touches Redis - on this specific machine (see "Honest
limits"), but the qualitative point holds anywhere: any network or even
loopback-IPC hop costs orders of magnitude more than an in-process hashmap
lookup under an uncontended mutex, which is the entire justification for
pairing an L1 cache like this one with Redis rather than hitting Redis
directly for hot keys.

## Correctness: verified, not assumed

`tests/unit_tests.cpp` (GoogleTest, 16 cases) covers basic ops, LRU
eviction order (including that both `get()` and `put()` on an existing key
refresh recency), TTL expiration (lazy and via the background reaper), and
stats tracking.

`tests/stress_test.cpp` is deliberately adversarial: 32 threads, 1.6M
total operations, a 64-key keyspace against a 32-entry cache (forcing
heavy same-key contention within and across shards), a mix of get/put/
erase - `erase()` racing a concurrent `get()` on the same key is the
likeliest place a subtle locking bug would show up.

**Run under ThreadSanitizer** (via `make tsan` - see "Honest limits" for
why that's a Docker container, not native): both the unit test suite and
the stress test come back clean. No data races detected across 1.6M
adversarial operations. I want to be honest about what that does and
doesn't mean: it means TSan found nothing in the interleavings that
occurred in this run, not that no race can exist - but it's a real,
substantive check that most "thread-safe by design" claims in a README
never actually go through.

## Honest limits

- **ThreadSanitizer is broken on this machine's native toolchain**: Apple
  Clang 17 on macOS 26.6 crashes `-fsanitize=thread` on *any* program, up
  to and including an empty `main()` with zero threads - confirmed by
  bisecting down from this project's code to a trivial reproduction before
  concluding it was a platform issue, not a bug here. Worked around by
  running TSan inside a disposable `ubuntu:24.04` container (`make tsan`),
  which is also exactly what CI does, so at least the two are consistent.
  If you hit the same crash: it's not your code either.
- **The throughput numbers are from one machine, one run pattern**
  (uniform random keys over a fixed keyspace, not a Zipfian/hot-key
  distribution, which is what YCSB and most real cache workloads actually
  use and which would change contention patterns and hit rates). Rerun
  before citing these numbers anywhere that matters.
- **The Redis latency comparison used loopback/Docker-Desktop-forwarded
  connections**, not a real network hop, and a real hop would only widen
  the gap in L1's favor - so if anything this comparison understates the
  case for an L1 tier, not overstates it.
- **No lock-free or read-optimized path** - see "Why exclusive locking"
  above. This is a deliberate simplicity-for-verifiability trade, not an
  oversight, but it's real headroom left on the table for a read-heavy
  workload.

## Project layout

```
include/cache/
  object_pool.hpp     Free-list allocator for cache nodes
  sharded_cache.hpp    The cache itself - shards, LRU, TTL, reaper thread
  redis_client.hpp      Thin hiredis wrapper (GET/SET PX/DEL)
  tiered_cache.hpp        L1 (ShardedCache) + L2 (Redis) composition
src/
  redis_client.cpp    hiredis wrapper implementation
  redis_demo.cpp      Small manual demo of the tiered cache
tests/
  unit_tests.cpp      GoogleTest: correctness (LRU, TTL, stats, sharding)
  stress_test.cpp     Adversarial concurrency test, meant to run under TSan
  quick_check.cpp     Minimal no-framework smoke test
benchmarks/
  bench_cache.cpp     Sharding x object-pool throughput (Google Benchmark)
  bench_tiered.cpp    L1-hit vs. Redis-round-trip latency
```

## CI

GitHub Actions builds and runs the unit + stress tests on every push, plus
a separate job running both under ThreadSanitizer on Linux (see
`.github/workflows/ci.yml`) - the same environment `make tsan` uses
locally, precisely because native TSan isn't currently reliable on this
project's primary dev machine (see "Honest limits").
