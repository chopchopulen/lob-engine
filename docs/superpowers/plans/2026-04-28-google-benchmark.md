# Google Benchmark Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `lob_bench` binary that reports P50 and P99 per-message latency in nanoseconds for individual `OrderBook` operations and the full add→book→features pipeline.

**Architecture:** Two new benchmark source files in `bench/` share the same compiled `OrderBook` and `FeatureEngine` object files as the main binary. Each fixture pre-warms a 10 k-order book with a rotating pool to keep map depth stable across millions of iterations. Per-iteration timing uses `std::chrono::high_resolution_clock`; P50/P99 are computed from a collected `std::vector<int64_t>` and reported via `state.counters`.

**Tech Stack:** C++17, Google Benchmark (system-wide install, `find_package`), CMake 3.16+

---

## File Map

| Action | Path | Responsibility |
|--------|------|----------------|
| Modify | `CMakeLists.txt` | Add `find_package(benchmark)` + `lob_bench` target |
| Create | `bench/bench_book.cpp` | Four fixtures: `BM_AddOrder`, `BM_DeleteOrder`, `BM_ReplaceOrder`, `BM_ExecuteOrder` |
| Create | `bench/bench_pipeline.cpp` | One fixture: `BM_FullPipeline` (add_order + on_book_update) |

No changes to any file under `src/` or `include/`.

---

## Task 1: Extend CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Append the benchmark target**

Open `CMakeLists.txt`. After the existing `add_executable(lob_engine …)` block, append:

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

- [ ] **Step 2: Verify CMake configure succeeds**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

Expected: configure completes with no errors. If `benchmark` is not found, install it:
```bash
brew install google-benchmark
```
Then re-run the cmake command.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add lob_bench target with google-benchmark"
```

---

## Task 2: Create bench/bench_book.cpp

**Files:**
- Create: `bench/bench_book.cpp`

This file benchmarks the four `OrderBook` operations in isolation. The book is pre-warmed with 10,000 bid orders spread across 200 price levels (50 orders per level). A rotating pool keeps the book at constant depth throughout the run.

**Price scheme:** order ref `r` (1-based) → price `9800 + (r-1) % 200`, side `'B'`, shares `100`.

- [ ] **Step 1: Create bench/bench_book.cpp**

```cpp
#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include <vector>
#include <algorithm>
#include <chrono>
#include <numeric>

// ── Shared setup ──────────────────────────────────────────────────────────────
// 10,000 bid orders across 200 price levels (50 orders/level).
// Price for ref r (1-based): 9800 + (r-1) % 200  (units: 1/10000 of a dollar)

static constexpr uint32_t POOL = 10'000;

static uint32_t price_for(uint64_t ref) {
    return 9800u + static_cast<uint32_t>((ref - 1) % 200);
}

static void fill_book(OrderBook& book) {
    for (uint64_t ref = 1; ref <= POOL; ++ref)
        book.add_order(ref, ref, 'B', 100, price_for(ref));
}

// ── P50/P99 helper ────────────────────────────────────────────────────────────
static void report_percentiles(benchmark::State& state,
                                std::vector<int64_t>& samples) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    state.counters["p50_ns"] = static_cast<double>(samples[samples.size() * 50 / 100]);
    state.counters["p99_ns"] = static_cast<double>(samples[samples.size() * 99 / 100]);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
// Note: bench_pipeline.cpp inlines this same logic rather than sharing a header (YAGNI).

// ── BM_AddOrder ───────────────────────────────────────────────────────────────
// Each iteration: delete ref[i % POOL] (untimed), then add it back (timed).
// Book stays at POOL orders throughout.
static void BM_AddOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<int64_t> samples;
    samples.reserve(2'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL) + 1;
        book.delete_order(ts++, ref);                          // evict (untimed)

        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(ts++, ref, 'B', 100, price_for(ref));  // timed
        auto t1 = std::chrono::high_resolution_clock::now();

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_AddOrder)->MinTime(2.0);

// ── BM_DeleteOrder ────────────────────────────────────────────────────────────
// Each iteration: delete ref[i % POOL] (timed), then re-add it (untimed).
static void BM_DeleteOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<int64_t> samples;
    samples.reserve(2'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL) + 1;

        auto t0 = std::chrono::high_resolution_clock::now();
        book.delete_order(ts++, ref);                          // timed
        auto t1 = std::chrono::high_resolution_clock::now();

        book.add_order(ts++, ref, 'B', 100, price_for(ref));  // replenish (untimed)

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_DeleteOrder)->MinTime(2.0);

// ── BM_ReplaceOrder ───────────────────────────────────────────────────────────
// Each iteration: replace live_refs[i % POOL] with a fresh ref (timed).
// live_refs tracks current refs so the book stays valid.
static void BM_ReplaceOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    // live_refs[i] = the order_ref currently occupying slot i in the pool
    std::vector<uint64_t> live_refs(POOL);
    std::iota(live_refs.begin(), live_refs.end(), uint64_t{1});

    std::vector<int64_t> samples;
    samples.reserve(2'000'000);

    uint64_t iter        = 0;
    uint64_t ts          = POOL + 1;
    uint64_t new_ref_seq = POOL + 1;  // monotonically increasing new refs

    for (auto _ : state) {
        uint64_t idx     = iter % POOL;
        uint64_t old_ref = live_refs[idx];
        uint64_t new_ref = new_ref_seq++;
        uint32_t price   = price_for(old_ref);

        auto t0 = std::chrono::high_resolution_clock::now();
        book.replace_order(ts++, old_ref, new_ref, 100, price);  // timed
        auto t1 = std::chrono::high_resolution_clock::now();

        live_refs[idx] = new_ref;  // keep tracking table current

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_ReplaceOrder)->MinTime(2.0);

// ── BM_ExecuteOrder ───────────────────────────────────────────────────────────
// Each iteration: execute ref[i % POOL] for 50 shares (timed), then
// delete + re-add to restore it to 100 shares (untimed).
static void BM_ExecuteOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<int64_t> samples;
    samples.reserve(2'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL) + 1;

        auto t0 = std::chrono::high_resolution_clock::now();
        book.execute_order(ts++, ref, 50);                     // timed (partial fill)
        auto t1 = std::chrono::high_resolution_clock::now();

        // Restore to 100 shares (untimed)
        book.delete_order(ts++, ref);
        book.add_order(ts++, ref, 'B', 100, price_for(ref));

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_ExecuteOrder)->MinTime(2.0);
```

- [ ] **Step 2: Build to verify compilation**

```bash
cmake --build build --target lob_bench 2>&1
```

Expected: compiles cleanly. Common errors and fixes:
- `benchmark/benchmark.h not found` → `brew install google-benchmark`, then re-run `cmake -B build`
- Missing symbol from `order_book.h` → check `target_include_directories` in CMakeLists.txt includes `include/`

- [ ] **Step 3: Run the book benchmarks to verify output**

```bash
./build/lob_bench --benchmark_filter="BM_Add|BM_Delete|BM_Replace|BM_Execute" \
                  --benchmark_min_time=1s
```

Expected: four rows of output, each showing `Time`, `CPU`, `Iterations`, `p50_ns`, `p99_ns`. Values will vary by machine; order of magnitude should be tens to hundreds of nanoseconds. If `p50_ns` or `p99_ns` is missing from a row, the `samples` vector was empty — check that the benchmark ran for at least one iteration.

- [ ] **Step 4: Commit**

```bash
git add bench/bench_book.cpp
git commit -m "bench: add per-operation OrderBook latency fixtures (P50/P99)"
```

---

## Task 3: Create bench/bench_pipeline.cpp

**Files:**
- Create: `bench/bench_pipeline.cpp`

Measures the hot path for the most common message type (Add = ~60% of real Nasdaq traffic): `book.add_order` + `features.on_book_update`. The book is pre-warmed with 5,000 bids and 5,000 asks so `top_of_book()` returns a valid spread and the feature engine computes real OFI deltas.

- [ ] **Step 1: Create bench/bench_pipeline.cpp**

```cpp
#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include "features/feature_engine.h"
#include <vector>
#include <algorithm>
#include <chrono>

// ── Setup ─────────────────────────────────────────────────────────────────────
// 5,000 bids at prices 9800–9999 (200 levels, 25 orders/level)
// 5,000 asks at prices 10001–10200 (200 levels, 25 orders/level)
// Gives a valid spread so FeatureEngine::on_book_update() runs full OFI logic.

static constexpr uint64_t BID_COUNT = 5'000;
static constexpr uint64_t ASK_COUNT = 5'000;

static uint32_t bid_price(uint64_t ref) {          // ref 1..BID_COUNT
    return 9800u + static_cast<uint32_t>((ref - 1) % 200);
}
static uint32_t ask_price(uint64_t ref) {          // ref BID_COUNT+1..BID_COUNT+ASK_COUNT
    return 10001u + static_cast<uint32_t>((ref - BID_COUNT - 1) % 200);
}

static void fill_book_pipeline(OrderBook& book) {
    for (uint64_t ref = 1; ref <= BID_COUNT; ++ref)
        book.add_order(ref, ref, 'B', 100, bid_price(ref));
    for (uint64_t ref = BID_COUNT + 1; ref <= BID_COUNT + ASK_COUNT; ++ref)
        book.add_order(ref, ref, 'S', 100, ask_price(ref));
}

// ── BM_FullPipeline ───────────────────────────────────────────────────────────
// Times one Add message flowing through the full book + feature stack.
// Per iteration:
//   1. delete ref[i % BID_COUNT] from the book (untimed — keeps depth stable)
//   2. book.add_order + features.on_book_update (timed)
//
// This is the dominant message type (~60% of ITCH traffic) and the exact
// hot path that runs 226M times on a full trading day.
static void BM_FullPipeline(benchmark::State& state) {
    OrderBook     book("TEST");
    FeatureEngine features;
    fill_book_pipeline(book);

    std::vector<int64_t> samples;
    samples.reserve(2'000'000);

    uint64_t iter = 0;
    // Start timestamp at 9:30 AM in nanoseconds (Nasdaq timestamps are
    // nanoseconds since midnight; 9:30 = 34,200 seconds)
    uint64_t ts = 34'200'000'000'000ULL;

    for (auto _ : state) {
        uint64_t ref = (iter % BID_COUNT) + 1;

        book.delete_order(ts++, ref);   // evict to keep book depth stable (untimed)

        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(ts, ref, 'B', 100, bid_price(ref));
        features.on_book_update(book, ts);
        auto t1 = std::chrono::high_resolution_clock::now();

        ts++;
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        state.counters["p50_ns"] =
            static_cast<double>(samples[samples.size() * 50 / 100]);
        state.counters["p99_ns"] =
            static_cast<double>(samples[samples.size() * 99 / 100]);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_FullPipeline)->MinTime(2.0);
```

- [ ] **Step 2: Build the full lob_bench binary**

```bash
cmake --build build --target lob_bench 2>&1
```

Expected: clean build. If `feature_engine.h` is missing includes, check `target_include_directories` in CMakeLists.txt.

- [ ] **Step 3: Run to verify output**

```bash
./build/lob_bench --benchmark_filter=BM_FullPipeline --benchmark_min_time=1s
```

Expected: one row showing `Time`, `CPU`, `Iterations`, `p50_ns`, `p99_ns`. The pipeline time should be higher than `BM_AddOrder` alone (it includes the feature engine update). If `p50_ns` is missing, the samples vector check failed — verify the loop ran.

- [ ] **Step 4: Commit**

```bash
git add bench/bench_pipeline.cpp
git commit -m "bench: add full pipeline latency fixture (add_order + on_book_update)"
```

---

## Task 4: Full run and validate

**Files:** none — run only

- [ ] **Step 1: Run all five benchmarks with stable settings**

```bash
./build/lob_bench \
  --benchmark_min_time=2s \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

Expected output shape (exact numbers will vary by machine):

```
Benchmark               Time     CPU   Iterations  items_per_second  p50_ns  p99_ns
BM_AddOrder/mean        XXX ns   ...               ...               ...     ...
BM_DeleteOrder/mean     XXX ns   ...               ...               ...     ...
BM_ReplaceOrder/mean    XXX ns   ...               ...               ...     ...
BM_ExecuteOrder/mean    XXX ns   ...               ...               ...     ...
BM_FullPipeline/mean    XXX ns   ...               ...               ...     ...
```

Sanity checks:
- `BM_ReplaceOrder` should be roughly `BM_DeleteOrder + BM_AddOrder` (it does both internally).
- `BM_FullPipeline` should be higher than `BM_AddOrder` alone (adds feature engine overhead).
- P99 should be 1.5–4× P50 (spikes from cache misses / branch mispredicts). If P99 > 10× P50, something is wrong with the rotating pool (book depth may be unstable).

- [ ] **Step 2: Save baseline numbers**

Copy the output into a comment block at the top of `bench/bench_book.cpp`:

```cpp
// ── Baseline (Apple M-series, 2026-04-28, Release -O2) ───────────────────────
// BM_AddOrder      p50=XXXns  p99=XXXns
// BM_DeleteOrder   p50=XXXns  p99=XXXns
// BM_ReplaceOrder  p50=XXXns  p99=XXXns
// BM_ExecuteOrder  p50=XXXns  p99=XXXns
// (see bench/bench_pipeline.cpp for BM_FullPipeline baseline)
```

And in `bench/bench_pipeline.cpp`:

```cpp
// ── Baseline (Apple M-series, 2026-04-28, Release -O2) ───────────────────────
// BM_FullPipeline  p50=XXXns  p99=XXXns
```

These baselines are the "before" numbers for roadmap item 6 (flat array replacement).

- [ ] **Step 3: Final commit**

```bash
git add bench/bench_book.cpp bench/bench_pipeline.cpp
git commit -m "bench: record latency baseline for std::map implementation"
```
