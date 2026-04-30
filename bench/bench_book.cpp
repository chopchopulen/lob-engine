#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include <vector>
#include <algorithm>
#include <chrono>
#include <numeric>

// ── Benchmark results (Apple Silicon, Release -O2) ───────────────────────────
// 200-level fixture (adversarial — overflow active throughout):
//   std::map:   AddOrder p50=41ns p99=42ns | DeleteOrder p50=42ns p99=83ns
//               ReplaceOrder p50=42ns p99=84ns | ExecuteOrder p50=41ns p99=42ns
//   FlatLevels: AddOrder p50=42ns p99=83ns | DeleteOrder p50=41ns p99=42ns
//               ReplaceOrder p50=42ns p99=84ns | ExecuteOrder p50=0ns p99=42ns
//
// 20-level fixture (realistic — flat array never full):
//   std::map:   AddOrder p50=41ns p99=42ns | DeleteOrder p50=41ns p99=42ns
//   FlatLevels: AddOrder p50=41ns p99=42ns | DeleteOrder p50=41ns p99=42ns
//   (see BM_FullPipeline_20L in bench_pipeline.cpp for the resume number)

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
    samples.reserve(10'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL) + 1;
        book.delete_order(ts++, ref);                          // evict (untimed)

        // Manual timing: PauseTiming()/ResumeTiming() have ~100ns overhead themselves;
        // high_resolution_clock is ~15ns on Apple Silicon — acceptable for ns-range ops.
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
    samples.reserve(10'000'000);

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
    samples.reserve(10'000'000);

    uint64_t iter        = 0;
    uint64_t ts          = POOL + 1;       // event timestamp counter
    uint64_t new_ref_seq = POOL + 1;       // fresh order refs; distinct from init pool 1..POOL

    for (auto _ : state) {
        uint64_t idx     = iter % POOL;
        uint64_t old_ref = live_refs[idx];
        uint64_t new_ref = new_ref_seq++;
        uint32_t price   = price_for(idx + 1);  // slot index is always 1..POOL

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
    samples.reserve(10'000'000);

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

// ── 20-level variants ─────────────────────────────────────────────────────────
// 1,000 bid orders across 20 price levels (50 orders/level).
// Prices 9980–9999: the flat array is never full, no demotion/promotion.
// This matches realistic ITCH data (10–30 active levels).

static constexpr uint32_t POOL_20 = 1'000;

static uint32_t price_for_20(uint64_t ref) {
    return 9980u + static_cast<uint32_t>((ref - 1) % 20);
}

static void fill_book_20(OrderBook& book) {
    for (uint64_t ref = 1; ref <= POOL_20; ++ref)
        book.add_order(ref, ref, 'B', 100, price_for_20(ref));
}

static void BM_AddOrder_20L(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book_20(book);

    std::vector<int64_t> samples;
    samples.reserve(10'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL_20 + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL_20) + 1;
        book.delete_order(ts++, ref);

        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(ts++, ref, 'B', 100, price_for_20(ref));
        auto t1 = std::chrono::high_resolution_clock::now();

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_AddOrder_20L)->MinTime(2.0);

static void BM_DeleteOrder_20L(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book_20(book);

    std::vector<int64_t> samples;
    samples.reserve(10'000'000);

    uint64_t iter = 0;
    uint64_t ts   = POOL_20 + 1;

    for (auto _ : state) {
        uint64_t ref = (iter % POOL_20) + 1;

        auto t0 = std::chrono::high_resolution_clock::now();
        book.delete_order(ts++, ref);
        auto t1 = std::chrono::high_resolution_clock::now();

        book.add_order(ts++, ref, 'B', 100, price_for_20(ref));

        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ++iter;
    }

    report_percentiles(state, samples);
}
BENCHMARK(BM_DeleteOrder_20L)->MinTime(2.0);
