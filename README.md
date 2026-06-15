# cpp-lob-engine

A high-performance **limit-order-book (LOB) matching engine** and market
simulator in modern C++20, with Python bindings and a microstructure-analytics
layer. The project pairs a deliberately-simple **reference** engine with a
cache-friendly **optimised** engine and proves they are observationally
identical on millions of messages — then benchmarks the gap.

---

## Why this design

- **Integer (tick) prices, never floating point on the match path** — matching
  is exact, deterministic and reproducible across machines.
- **Reference vs Fast split** — `OrderBookRef` (`std::map` + `std::list`) is the
  obviously-correct oracle; `OrderBookFast` (flat tick-indexed level array +
  intrusive lists + object pool) is the production path. A *golden* test replays
  identical streams through both and asserts byte-identical trade tapes, book
  updates and snapshots.
- **Allocation-free steady state** — order nodes come from a pre-sized pool /
  free-list; no per-message heap traffic.
- **Deterministic simulation** — a single seeded PRNG drives the synthetic flow,
  so any run is exactly reproducible (and unit-tested to be so).

## Architecture

```
                 normalized Message stream
                          │
        ┌─────────────────┴──────────────────┐
        │                                     │
  OrderBookRef                          OrderBookFast
  std::map<Price,Level>                 level[] indexed by tick
  std::list per level (FIFO)            intrusive list + object pool
        │                                     │
        └──────────────► EventSink ◄──────────┘
             (on_trade / on_book_update; no I/O in core)
                          │
        ┌─────────────────┼───────────────────┐
   GoogleTest         Benchmarks           pybind11 (lobpy)
   unit / golden /    latency p50/99/99.9      │
   fuzz               + throughput        analytics: mid, micro-price,
                                          OFI, imbalance, realized spread,
                                          mark-outs, Lee-Ready
```

Repository layout:

| Path          | Contents                                                        |
|---------------|-----------------------------------------------------------------|
| `include/lob/`| Public headers: types, sinks, `OrderBookRef`, `OrderBookFast`, generator, replay |
| `src/`        | Generator + CSV/replay implementation                           |
| `tests/`      | GoogleTest: unit, CSV round-trip, **golden Ref==Fast**, fuzz    |
| `bench/`      | Latency/throughput harness + Google Benchmark microbenchmarks   |
| `python/`     | pybind11 module `lobpy`                                          |
| `analytics/`  | Microstructure feature scripts + Jupyter notebook               |
| `data/`       | Sample message CSV, synthetic generator, LOBSTER drop-in notes  |
| `docs/`       | `DESIGN.md`, `BENCHMARKS.md`, generated plots                   |

## Build & run (C++)

Requires a C++20 compiler, CMake ≥ 3.20. GoogleTest / Google Benchmark /
pybind11 are fetched automatically by CMake (`FetchContent`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# run the tests (includes the golden Ref==Fast cross-validation + fuzz)
ctest --test-dir build --output-on-failure

# latency / throughput harness (writes docs/bench_results.csv)
./build/lob_bench_harness 2000000 7

# Google Benchmark microbenchmarks
./build/lob_microbench
```

### Sanitizer build

```bash
cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## Build & run (Python)

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cmake -S . -B build && cmake --build build -j        # builds lobpy
export PYTHONPATH=$PWD/build:$PYTHONPATH

python analytics/run_analytics.py                    # writes features + plots
jupyter nbconvert --to notebook --execute analytics/microstructure.ipynb
```

```python
import lobpy
sim = lobpy.simulate(n=200_000, seed=7)   # dict of numpy arrays
print(sim["trades"]["price"][:5])
```

## Benchmarks

Headline numbers (Apple M-series, Apple clang, `-O3`) are in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) together with the methodology and a
latency/throughput plot. The optimised engine is several times faster than the
reference on identical workloads while producing identical output.

## Limitations / future work

- Single instrument, single thread (matching is inherently sequential; the
  realistic scaling axis is one shard per symbol).
- Tick domain is bounded at construction (a deliberate space/time trade-off);
  see `docs/DESIGN.md`.
- Self-trade prevention, iceberg/hidden orders and pro-rata matching are not
  modelled (price-time only). These are natural extensions.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the full rationale and complexity
analysis.
