# Design notes

This document is the interview-oriented rationale for `cpp-lob-engine`: the
*why* behind each decision, the complexity analysis, and how correctness,
determinism and performance are established. It is meant to be defensible
line-by-line.

## Toolchain & standard

| Item        | Choice (dev machine)                                |
|-------------|------------------------------------------------------|
| OS          | macOS 15.7 (Darwin 24.6, arm64)                      |
| Compiler    | Apple Clang 17.0.0 (`clang-1700`)                    |
| C++ standard| **C++20** (`-std=c++20`, no GNU extensions)          |
| Build       | CMake ≥ 3.20, Make/Ninja                             |
| Test / bench| GoogleTest v1.15.2, Google Benchmark v1.9.0 (FetchContent) |
| Bindings    | pybind11 v2.13.6 (FetchContent), CPython 3.9         |

**C++20** was chosen because Apple Clang 17 supports the subset we use cleanly
(`std::optional`, `<charconv>` integer parsing, designated initializers,
structured bindings, `if constexpr`-free templated engines). Nothing here needs
C++23; staying at 20 maximises portability to the GCC 12 / Clang 15 baselines in
CI. If only an older compiler were available the code is C++17-compatible apart
from a few designated-initializer uses.

All first-party targets compile with `-Wall -Wextra -Wpedantic -Werror`. A
separate sanitizer configuration (`-DLOB_SANITIZE=ON`) adds ASan + UBSan and the
full test suite is kept clean under it — this is what makes the intrusive-list /
object-pool memory management trustworthy.

## Domain model

- `Price` is `int32_t` **in ticks**. `Quantity` is `int64_t`. `OrderId` /
  `Timestamp` are `uint64_t`.
- A single normalized `Message` (Limit / Market / Cancel / Modify) is the only
  input type. Both engines and the CSV format speak it, so external feeds plug in
  by conversion, not by special-casing the core.
- The core emits two event types through a templated **sink**: `Trade` (one per
  fill, always at the *maker's* price) and `BookUpdate` (new aggregate size at a
  level whenever it changes; `0` ⇒ level removed).

### Why integer tick prices (no floating point)

Floating-point prices make matching non-deterministic and bug-prone: `==` on
doubles is unsafe, accumulation drifts, and results can differ across compilers /
FMA settings. Exchanges quote on a discrete tick grid, so the natural and correct
representation is an integer count of ticks. Consequences:

- Price comparison, crossing checks and level indexing are exact integer ops.
- The optimised engine can use the tick value **directly as an array index**.
- Runs are bit-for-bit reproducible — essential for the golden test and for any
  research/backtest that must be replayable.

Conversion to/from currency is a presentation concern handled outside the hot
path.

## Reference vs Fast: why two engines

`OrderBookRef` exists to be *obviously correct*, not fast:

- `std::map<Price, Level, Compare>` — sorted levels, `O(log L)` best-price &
  insert, where `L` = number of occupied levels.
- `std::list<Order>` per level — `O(1)` FIFO push-back and pop-front, stable
  iterators so an `id → iterator` index makes cancel `O(1)` (plus the map lookup).

`OrderBookFast` is the production path with the identical public interface and —
critically — the identical *observable behaviour*:

- **Flat level array indexed by tick**: `levels_[price]`. Best-price lookup and
  level access are `O(1)`, branch-light and cache-friendly. Cost: memory is
  `O(tick_domain)` and the domain is bounded at construction.
- **Intrusive doubly-linked list per level**: order nodes carry `prev/next`
  indices, so cancel from the middle of a queue is `O(1)` with no search.
- **Object pool + free list**: nodes are slots in a pre-reserved `std::vector`;
  `alloc`/`free` are vector pop/push of an index. After warm-up the steady state
  performs **zero heap allocations** per message.
- **Cached best bid/ask**: maintained on insert; only re-scanned when the top
  level is fully consumed (amortised cheap; see below).

### Complexity comparison

| Operation            | OrderBookRef        | OrderBookFast                    |
|----------------------|---------------------|----------------------------------|
| add (no cross)       | `O(log L)`          | `O(1)`                           |
| add (crossing k lvls)| `O(k + log L)`      | `O(k + Δ)`                       |
| cancel               | `O(1)` + map lookup | `O(1)` + hash lookup             |
| modify               | cancel + add        | cancel + add                     |
| best bid/ask         | `O(1)` (map begin)  | `O(1)` (cached)                  |
| top-N snapshot       | `O(N)`              | `O(N + gaps)`                    |

`Δ` is the distance scanned to find the next non-empty best level after the top
is emptied. A naïve worst case is `O(tick_domain)`, but each tick is scanned at
most once per "regime" and re-population resets it, so across a realistic stream
the best-price maintenance is amortised small. The narrow-tick-domain golden test
(`num_ticks = 256`) deliberately stresses this path.

The `id → slot` map is `std::unordered_map`. With dense ids a flat `vector` index
would make cancel truly `O(1)` allocation-free; the hash map is the pragmatic
choice for arbitrary 64-bit ids and is pre-reserved so it does not allocate in
steady state. This trade-off is called out honestly rather than hidden.

## Determinism

- The synthetic generator is driven by a single `std::mt19937_64` seeded from the
  config. All sampling uses our own integer/`[0,1)` helpers rather than
  `std::*_distribution`, whose output is **not** specified across standard-library
  implementations — so "same seed ⇒ identical stream" holds even across
  platforms, and is asserted by `Generator.SameSeedSameStream`.
- Matching contains no floating point, no time-dependent branching, no hashing of
  pointers — given the same input both engines produce the same output every run.

## Correctness validation strategy

1. **Unit tests** (`test_ref_engine.cpp`) pin the matching semantics: price-time
   priority, partial fills, FIFO within a level, multi-level crossing, market
   sweep + residual discard, cancel, cancel-replace priority loss, empty-book and
   zero-qty edges, and book invariants.
2. **CSV round-trip** (`test_csv.cpp`): serialize → parse → replay yields an
   identical tape, validating the external-data seam.
3. **Golden cross-validation** (`test_golden_crossval.cpp`) — the central story.
   The same synthetic and CSV streams are replayed through `OrderBookRef` and
   `OrderBookFast`; we assert the **full trade tape**, the **full book-update
   stream**, and the **final snapshot** are identical, across many seeds and
   stress configs (market-heavy, thin book, narrow tick domain).
4. **Fuzz / property test** (`test_fuzz.cpp`): 25 seeds × 20k messages, checking
   Ref==Fast *after every message* and periodically asserting invariants. Run
   under ASan/UBSan this also certifies memory safety of the pool and lists.

The two engines emit events at the **same points in the same order** by
construction: `OrderBookFast::match_*` mirrors `OrderBookRef::match_against`
line-for-line (one book update per maker level after it is processed; trades in
execution order; one update when residual rests). That shared contract is what
makes equality, not just "same final book", the assertion.

## Benchmark methodology

See `docs/BENCHMARKS.md` for the numbers. In summary:

- `bench/bench_harness.cpp`: builds one workload, warms caches/predictors, then
  measures **throughput** (median of *N* full-stream replays, messages/sec) and
  **per-message latency** sampled with `std::chrono::steady_clock`, reported as
  p50 / p90 / p99 / p99.9 / max. The latency figure *includes* timer overhead
  (~tens of ns on this machine); this is stated rather than subtracted.
- `bench/bench_engines.cpp`: Google Benchmark microbenchmarks for full replay and
  for add/cancel churn, giving stable per-op timings with its own statistics.
- The Python layer (`analytics/plot_bench.py`) renders the CSV the harness writes
  into a latency/throughput figure under `docs/`.

## Analytics layer

`lobpy.simulate()` replays synthetic flow through the optimised engine and
returns, as numpy columns, the per-message L1 (best bid/ask + sizes) and the full
trade tape. From those `analytics/features.py` computes standard microstructure
features: mid-price, **micro-price** (size-weighted), **order-flow imbalance**
(OFI), queue/volume imbalance, **realized spread**, multi-horizon **mark-outs**,
and **Lee-Ready** trade-sign classification. The notebook visualises depth over
time, OFI vs short-horizon returns, and mark-out curves.

## Things deliberately *not* done

- Multi-threading: matching a single book is sequential; the real concurrency
  axis is sharding by symbol, out of scope for one instrument.
- Self-trade prevention, hidden/iceberg quantity, pro-rata or size-priority
  matching: price-time only, to keep the correctness story crisp.
- Network/persistence: the core is pure compute with no I/O, by design.
