# Google Benchmark Integration — Design Spec
**Date:** 2026-04-28  
**Scope:** Add per-message latency measurement (P50, P99 in nanoseconds) using Google Benchmark

---

## Goal

Measure per-message median and 99th-percentile latency for:
1. Individual `OrderBook` operations in isolation (add, delete, replace, execute)
2. The full end-to-end pipeline (parse raw ITCH bytes → book update → feature computation)

These benchmarks also serve as the baseline for item 6 on the roadmap (replacing `std::map` with a flat sorted array).

---

## Assumptions

- Google Benchmark is installed system-wide (e.g. `brew install google-benchmark`).
- No changes to existing source files (`itch_parser.cpp`, `order_book.cpp`, `feature_engine.cpp`, `main.cpp`).
- C++17, Release build (`-O2`).

---

## File Layout

```
bench/
  bench_book.cpp        # 4 fixtures: add_order, delete_order, replace_order, execute_order
  bench_pipeline.cpp    # 1 fixture: full parse → book → features pipeline
CMakeLists.txt          # extended with find_package(benchmark) + lob_bench target
```

No new headers. No changes to `src/` or `include/`.

---

## CMake Changes

Append to the existing `CMakeLists.txt`:

```cmake
# ── Google Benchmark ──────────────────────────────────────────────────────────
find_package(benchmark REQUIRED)

add_executable(lob_bench
    bench/bench_book.cpp
    bench/bench_pipeline.cpp
    src/feed/itch_parser.cpp
    src/book/order_book.cpp
    src/features/feature_engine.cpp
)
target_include_directories(lob_bench PRIVATE include)
target_link_libraries(lob_bench PRIVATE benchmark::benchmark_main)
```

`benchmark::benchmark_main` provides `main()`, so no manual entry point is needed. The benchmark binary reuses the same source files as `lob_engine` — no code duplication.

---

## `bench/bench_book.cpp`

### Setup (shared across all four fixtures)

Pre-warm an `OrderBook("TEST")` with **10,000 orders** spread across ~200 price levels. This puts `std::map` at realistic operating depth — matching what AAPL/AMZN look like at mid-session.

Use a **rotating pool** of 10,000 pre-generated order refs (`uint64_t` sequence 1…10000). Each iteration:
- Deletes `pool[i % 10000]` from the book
- Re-adds it (or performs the operation under test)

This keeps book size constant throughout the run, preventing the map from growing and giving artificially cheap early iterations.

### Fixtures

| Benchmark | Operation timed | Per-iteration book mutation |
|---|---|---|
| `BM_AddOrder` | `book.add_order(...)` | delete oldest ref, re-add it |
| `BM_DeleteOrder` | `book.delete_order(...)` | delete ref, re-add it to replenish |
| `BM_ReplaceOrder` | `book.replace_order(...)` | replace ref in-place (old_ref → new_ref, same side, slightly different price) |
| `BM_ExecuteOrder` | `book.execute_order(...)` | partial fill (half the shares); when order is fully consumed, re-add it |

### Latency recording

Inside each loop iteration, record the individual sample:

```cpp
auto t0 = std::chrono::high_resolution_clock::now();
// operation under test
auto t1 = std::chrono::high_resolution_clock::now();
samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
```

After the loop, sort `samples` and emit:

```cpp
state.counters["p50_ns"] = (double)samples[samples.size() * 50 / 100];
state.counters["p99_ns"] = (double)samples[samples.size() * 99 / 100];
state.SetItemsProcessed(state.iterations());
```

`high_resolution_clock` overhead on Apple Silicon is ~15–20 ns. Operations are expected to be 50–400 ns, so clock overhead is 5–20% — acceptable and honest.

---

## `bench/bench_pipeline.cpp`

### Setup

Build an **in-memory buffer** of 10,000 raw ITCH 5.0 message bytes (no file I/O in the hot path). Message mix mirrors real Nasdaq data:

| Type | % | Bytes |
|---|---|---|
| Add (`A`) | 60% | 36 |
| Delete (`D`) | 25% | 19 |
| Replace (`U`) | 10% | 35 |
| Execute (`E`) | 5% | 23 |

Each message is prefixed with the 2-byte length field as the real format requires. Stock field is set to `"TEST    "` (8 bytes, space-padded). Prices and order refs are generated to produce a valid, non-degenerate book state.

Wire up `ParserCallbacks → OrderBook → FeatureEngine` exactly as `main.cpp` does.

### Fixture

`BM_FullPipeline` iterates over the message buffer repeatedly, dispatching each message through the full stack. Per-message latency is recorded and P50/P99 reported the same way as `bench_book.cpp`.

---

## Running

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target lob_bench
./build/lob_bench --benchmark_min_time=2s --benchmark_repetitions=5
```

Expected output shape:

```
Benchmark               Time     CPU   Iterations  p50_ns  p99_ns
BM_AddOrder           142 ns   141 ns    4823091    138     210
BM_DeleteOrder        165 ns   164 ns    4102847    161     244
BM_ReplaceOrder       290 ns   289 ns    2381004    283     401
BM_ExecuteOrder       158 ns   157 ns    4298763    155     230
BM_FullPipeline       380 ns   378 ns    1847291    371     520
```

---

## Future use (item 6)

When `std::map` is replaced with a flat sorted array, rerun `lob_bench` with no other changes. The `BM_Add/Delete/Replace/Execute` numbers will show the before/after directly.
