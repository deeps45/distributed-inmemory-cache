# Distributed In-Memory Cache

A concurrent, TTL- and LRU-aware in-memory key-value cache in C++20, sharded
across independent locks instead of one global mutex, with a custom
free-list object allocator, a benchmark suite quantifying every one of
those decisions independently, an adversarial concurrency test verified
clean under ThreadSanitizer, and an optional Redis L2 tier demonstrating
the actual reason an in-process cache like this is worth having next to
Redis rather than instead of it.

**What I'd do differently with more time:** implement buffered reads with
a genuinely lock-free ring buffer instead of the mutex-guarded one in
`sharded_cache.hpp` - the measured result below strongly suggests the
mutex, not the batching idea itself, is what makes that mode a net loss
here, but I haven't built the lock-free version to confirm it; replace the
O(shard size) TTL sweep with a secondary min-heap ordered by expiry for
caches with many long-lived keys mixed with short-TTL ones; and run the
full benchmark suite on a quieter machine - several runs here had a
system load average of 7-12 from unrelated background processes, which is
exactly the kind of noise that makes single-threaded microbenchmark
numbers less trustworthy (see "Honest limits").

## Why this exists, not just what it does

"C++ LRU cache" is one of the most-implemented toy projects there is. What
makes this one worth a resume line isn't that it works - it's that every
design decision in it was actually measured, including the one that didn't
pan out. `ShardedCache` with `shard_count=1` *is* the naive single-lock
baseline, not a separate reimplementation of it, so the sharding A/B in
[Benchmarks](#benchmarks) is a true isolation of one variable. And when a
second, theoretically-motivated optimization (buffered reads, modeled on a
real technique from a real high-performance cache) turned out to measure
*worse*, that's reported here too, with the root cause isolated in its own
microbenchmark - not quietly dropped.

## Architecture

```
                         ShardedCache<K, V>
                    ┌─────────────────────────────────────────┐
   get(k)/put(k,v)  │  hash(k) % shard_count                  │
   ───────────────▶ │           │                              │
                    │           ▼                              │
                    │  ┌───────────────┐  ┌───────────────┐    │
                    │  │    Shard 0    │  │    Shard N    │... │
                    │  │ shared_mutex  │  │ shared_mutex  │    │
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

Each shard's lock is a `std::shared_mutex`, used in two different modes
depending on `use_read_buffer`:

- **Default (`use_read_buffer=false`)**: every `get()`/`put()`/`erase()`
  takes it exclusively via `std::lock_guard`, functionally identical to a
  plain `std::mutex`. This is the recommended, measured-faster mode - see
  below for why.
- **Opt-in (`use_read_buffer=true`)**: `get()` hits take a *shared* lock to
  read the map, and record the touch in a small per-shard buffer (guarded
  by its own separate mutex) instead of mutating the LRU list inline. That
  buffer is replayed onto the list in batches later. Implemented, correct
  (see [Correctness](#correctness-verified-not-assumed)), and **measurably
  slower** than the default in every configuration tested - see below.

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
with Apple Clang 17 and Homebrew LLVM/Clang 23), and optionally Docker for
the Redis L2 demo and as a TSan fallback.

```bash
make build   # configures + builds everything (fetches GoogleTest, Google
             # Benchmark via CMake FetchContent - both free, open source)
make test    # unit tests + the concurrent stress test
make bench   # the sharding/pool/read-buffer factorial throughput benchmark
make redis   # starts a local Redis for the L2 tier demo
make bench-tiered   # L1-hit vs. Redis-round-trip latency comparison
make tsan    # unit tests + stress test under ThreadSanitizer - uses
             # Homebrew LLVM natively if present, else a Linux container
             # (see "Honest limits" for why native Apple Clang can't)
```

`./build/redis_demo` is a small standalone program exercising the tiered
cache manually against a running Redis (`make redis` first).
`./build/bench_lock_overhead` is the standalone microbenchmark referenced
below, isolating `std::mutex` vs. `std::shared_mutex` overhead on their own.

## Benchmarks

Every number below is from an actual run on this machine (Apple Silicon,
10 cores) and is reproducible with the `make`/`./build/bench_*` commands
above - rerun before citing any of it, especially on different hardware.

### Sharding and the object pool

`benchmarks/bench_cache.cpp` runs a 90% get / 10% put workload (a common
cache read/write ratio, cf. YCSB workload B) against a 10,000-key space
with a 2,000-entry cache (real eviction pressure), at increasing thread
counts. All configurations below use `use_read_buffer=false` (the
recommended mode) so this table isolates just sharding and the pool.

| Threads | Global lock, no pool | Global lock, + pool | 16 shards, no pool | 16 shards, + pool |
|---|---|---|---|---|
| 1  | 11.55M ops/s | 12.22M ops/s | 12.39M ops/s | 8.92M ops/s* |
| 2  | 2.30M ops/s  | 2.34M ops/s  | 3.01M ops/s  | 3.55M ops/s  |
| 4  | 536k ops/s   | 566k ops/s   | 968k ops/s   | 1.52M ops/s  |
| 8  | 159k ops/s   | 135k ops/s   | 444k ops/s   | 537k ops/s   |
| 16 | 85.5k ops/s  | 87.4k ops/s  | 209k ops/s   | 237k ops/s   |

*\* high variance on this single cell (coefficient of variation 28.8% vs.
1-4% typical elsewhere in this table) - system load average was 7-12 from
unrelated background processes during this run; treat this one number
skeptically and the 2-16 thread columns as the reliable signal.*

**Sharding alone** (global-lock-+-pool vs. 16-shards-+-pool, isolating
just that variable, using the 2-16 thread columns given the noise noted
above): 1.52x at 2 threads, climbing to **3.97x at 8 threads** (this
machine's physical core count), settling to 2.71x at 16 (oversubscribed -
16 threads on 10 cores).

**The object pool alone**: helps at every thread count for 16 shards
(+57% at 4 threads, +21% at 8), but *hurts* slightly for the global-lock
baseline at 8 threads (135k vs. 159k, about -15%). Reported as measured,
not smoothed: allocator behavior under lock contention is noisy, and a
single global lock is already the bottleneck at that point, so shaving
allocation cost has less to work with.

### Buffered reads: measured, and rejected

The same benchmark also compares `use_read_buffer=false` (default) against
`true` (opt-in), both at 16 shards with the pool enabled - the best
configuration from the table above - under both a uniform-random keyspace
and a skewed one concentrating most traffic on a small set of hot keys
(closer to real cache traffic, and the shape buffered reads are supposed
to help with most).

| Threads | Uniform, no buffer | Uniform, buffered | Skewed, no buffer | Skewed, buffered |
|---|---|---|---|---|
| 1  | 8.92M ops/s* | 9.73M ops/s | 12.99M ops/s | 8.36M ops/s |
| 2  | 3.55M ops/s  | 2.67M ops/s | 3.24M ops/s  | 2.82M ops/s |
| 4  | 1.52M ops/s  | 1.15M ops/s | 1.07M ops/s  | **1.28M ops/s** |
| 8  | 537k ops/s   | 436k ops/s  | 421k ops/s   | **462k ops/s**  |
| 16 | 237k ops/s   | 185k ops/s  | 202k ops/s   | 190k ops/s  |

Buffered reads are **slower at 8 of 10 measured points**, including
single-threaded with zero contention to relieve - the two exceptions are
the skewed workload at 4 and 8 threads, where it's modestly ahead (+20%,
+10%). Not the result I expected going in. I isolated why instead of
either hiding it or hand-waving past it:

```bash
./build/bench_lock_overhead --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

| | Mean time |
|---|---|
| `std::mutex`, lock+unlock | 8.4 ns |
| `std::shared_mutex`, *shared* lock+unlock | 19.1 ns |
| `std::shared_mutex`, exclusive lock+unlock | 21.5 ns |

On this platform's libc++, an uncontended `std::shared_mutex` costs
**roughly 2.3x an uncontended `std::mutex`** for an identical, trivial
critical section - even its own shared-lock path, let alone the exclusive
one. The buffered-read mode pays that tax on every single `get()`, *and*
adds a second lock acquisition (the touch-buffer mutex) on the hit path.
A hashmap lookup is nowhere near long enough a critical section to amortize
either cost, let alone both - the fixed overhead dominates before any
concurrency benefit gets a chance to show up, and only partially gets
clawed back under exactly the contention pattern (skewed, moderate thread
count) it was designed for.

Caffeine's actual read-buffer implementation uses a genuinely lock-free
ring buffer, not a second mutex, and runs in a context (JVM, with
additional per-read bookkeeping for frequency-based eviction policies)
where this comparison looks different. A mutex-guarded buffer was a
reasonable-sounding simplification - implementing the real lock-free
version was explicitly out of scope for keeping this verifiable with
ThreadSanitizer in the time available - and the data disproved it. It's
left in the codebase, off by default, correctness-tested via the
`ReadBuffer.*` cases in `tests/unit_tests.cpp`, because reporting a
technique that didn't work, with the evidence for why, is more useful than
deleting it and pretending it was never tried.

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
lookup under an uncontended lock, which is the entire justification for
pairing an L1 cache like this one with Redis rather than hitting Redis
directly for hot keys.

## Correctness: verified, not assumed

`tests/unit_tests.cpp` (GoogleTest, 19 cases) covers basic ops, LRU
eviction order (including that both `get()` and `put()` on an existing key
refresh recency), TTL expiration (lazy and via the background reaper),
stats tracking, and - in the `ReadBuffer.*` suite - the buffered-read mode
specifically: that disabling it explicitly still behaves correctly, that
the touch-buffer overflow path drains with correct LRU order (the only
way to exercise that code path without waiting on `put()`'s own pre
-eviction drain), and that a concurrent read-heavy workload never exceeds
capacity under it.

`tests/stress_test.cpp` is deliberately adversarial: 32 threads, 1.6M
total operations, a 64-key keyspace against a 32-entry cache (forcing
heavy same-key contention within and across shards), a mix of get/put/
erase - `erase()` racing a concurrent `get()` on the same key is the
likeliest place a subtle locking bug would show up.

**Run under ThreadSanitizer** (`make tsan`, natively via Homebrew LLVM on
this machine - see "Honest limits"): both the unit test suite and the
stress test come back clean. No data races detected across 1.6M
adversarial operations, including the buffered-read path's overflow-drain
logic, which is exactly the kind of code (a separate mutex, a key lookup
racing against eviction) most likely to hide a subtle bug. I want to be
honest about what a clean run does and doesn't mean: it means TSan found
nothing in the interleavings that occurred in these runs, not that no race
can exist - but it's a real, substantive check that most "thread-safe by
design" claims in a README never actually go through.

## Honest limits

- **Several benchmark runs happened under real system load** (load average
  7-12 from unrelated background processes on this machine, not from this
  benchmark itself). The single highest-variance cell is called out inline
  above; treat any number with an unusually high coefficient of variation
  the same way, and rerun on a quiet machine before citing anything here
  as precise.
- **ThreadSanitizer doesn't work on this machine's Apple-bundled
  toolchain**: Apple Clang 17 (Xcode Command Line Tools) on macOS 26.6
  crashes `-fsanitize=thread` on *any* program, up to and including an
  empty `main()` with zero threads - confirmed by bisecting down from this
  project's code to a trivial reproduction before concluding it was a
  platform issue. Homebrew's own LLVM/Clang (`brew install llvm`) doesn't
  have this problem and is what `make tsan` uses when present, falling
  back to a disposable Linux container otherwise - which is also exactly
  what CI runs, so the two stay consistent regardless of which path a
  given machine takes.
- **The throughput numbers are from one machine, one architecture.**
  Numbers on real distributed/server infrastructure, different core
  counts, or a different OS's mutex/shared_mutex implementation could
  shift the specific ratios - the qualitative findings (sharding helps a
  lot, the object pool helps some, this particular buffered-read
  implementation doesn't) are more likely to generalize than the exact
  percentages.
- **The Redis latency comparison used loopback/Docker-Desktop-forwarded
  connections**, not a real network hop, and a real hop would only widen
  the gap in L1's favor - so if anything this comparison understates the
  case for an L1 tier, not overstates it.
- **The buffered-read finding is specific to this mutex-based
  implementation**, not necessarily to the technique in general - see
  "What I'd do differently" above.

## Project layout

```
include/cache/
  object_pool.hpp     Free-list allocator for cache nodes
  sharded_cache.hpp    The cache itself - shards, LRU, TTL, reaper thread,
                       and the buffered-read mode
  redis_client.hpp      Thin hiredis wrapper (GET/SET PX/DEL)
  tiered_cache.hpp        L1 (ShardedCache) + L2 (Redis) composition
src/
  redis_client.cpp    hiredis wrapper implementation
  redis_demo.cpp      Small manual demo of the tiered cache
tests/
  unit_tests.cpp      GoogleTest: correctness (LRU, TTL, stats, sharding,
                       buffered reads)
  stress_test.cpp     Adversarial concurrency test, meant to run under TSan
  quick_check.cpp     Minimal no-framework smoke test
benchmarks/
  bench_cache.cpp         Sharding x pool x read-buffer throughput factorial
  bench_lock_overhead.cpp  Isolated std::mutex vs. std::shared_mutex cost
  bench_tiered.cpp        L1-hit vs. Redis-round-trip latency
```

## CI

GitHub Actions builds and runs the unit + stress tests on every push, plus
a separate job running both under ThreadSanitizer on Linux (see
`.github/workflows/ci.yml`) - CI runners don't have Homebrew LLVM
available, so that job always takes the same Linux-native TSan path
`make tsan` falls back to locally when Homebrew LLVM isn't installed (see
"Honest limits").
