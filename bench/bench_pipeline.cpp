#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include "features/feature_engine.h"
#include <vector>
#include <algorithm>
#include <chrono>

// ── Benchmark results (Apple Silicon, Release -O2) ───────────────────────────
// Full pipeline hot path: add_order + on_book_update per message.
//
// 200-level fixture (adversarial — overflow active throughout):
//   std::map:   BM_FullPipeline  p50=42ns  p99=84ns
//   FlatLevels: BM_FullPipeline  p50=42ns  p99=84ns  (no improvement: overflow dominates)
//
// 20-level fixture (realistic — 10–30 active levels, flat array never full):
//   std::map:   BM_FullPipeline_20L  mean=58ns  p50=42ns  p99=42ns
//   FlatLevels: BM_FullPipeline_20L  mean=52ns  p50=41ns  p99=42ns  ← -11% / -6ns
//
// The 20-level number is the one that represents live trading performance.

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
//
// Manual timing: PauseTiming()/ResumeTiming() have ~100ns overhead themselves;
// high_resolution_clock is ~15ns on Apple Silicon — acceptable for ns-range ops.
static void BM_FullPipeline(benchmark::State& state) {
    OrderBook     book("TEST");
    FeatureEngine features;
    fill_book_pipeline(book);

    std::vector<int64_t> samples;
    samples.reserve(10'000'000);

    uint64_t iter = 0;
    // Start timestamp at 9:30 AM in nanoseconds (Nasdaq timestamps are
    // nanoseconds since midnight; 9:30 = 34,200 seconds)
    uint64_t ts = 34'200'000'000'000ULL;

    for (auto _ : state) {
        uint64_t ref = (iter % BID_COUNT) + 1;

        // Only the bid side is rotated; asks remain static to isolate bid-add latency.
        book.delete_order(ts++, ref);   // evict to keep bid depth stable (untimed)

        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(ts, ref, 'B', 100, bid_price(ref));
        features.on_book_update(book, ts);  // add_order and on_book_update share ts — same event
        auto t1 = std::chrono::high_resolution_clock::now();

        ts++;  // deferred past t1 to keep timed region side-effect-free
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

// ── BM_FullPipeline_20L ───────────────────────────────────────────────────────
// Same pipeline benchmark with 20 bid levels (9990–10009) + 20 ask levels
// (10011–10030). Flat array is never full — no demotion/promotion overhead.
// This is the realistic case for live ITCH data.

static constexpr uint64_t BID_COUNT_20 = 500;   // 500 bids × 20 levels = 25/level
static constexpr uint64_t ASK_COUNT_20 = 500;

static uint32_t bid_price_20(uint64_t ref) {
    return 9990u + static_cast<uint32_t>((ref - 1) % 20);
}
static uint32_t ask_price_20(uint64_t ref) {
    return 10011u + static_cast<uint32_t>((ref - BID_COUNT_20 - 1) % 20);
}

static void fill_book_pipeline_20(OrderBook& book) {
    for (uint64_t ref = 1; ref <= BID_COUNT_20; ++ref)
        book.add_order(ref, ref, 'B', 100, bid_price_20(ref));
    for (uint64_t ref = BID_COUNT_20 + 1; ref <= BID_COUNT_20 + ASK_COUNT_20; ++ref)
        book.add_order(ref, ref, 'S', 100, ask_price_20(ref));
}

static void BM_FullPipeline_20L(benchmark::State& state) {
    OrderBook     book("TEST");
    FeatureEngine features;
    fill_book_pipeline_20(book);

    std::vector<int64_t> samples;
    samples.reserve(10'000'000);

    uint64_t iter = 0;
    uint64_t ts = 34'200'000'000'000ULL;

    for (auto _ : state) {
        uint64_t ref = (iter % BID_COUNT_20) + 1;

        book.delete_order(ts++, ref);

        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(ts, ref, 'B', 100, bid_price_20(ref));
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
BENCHMARK(BM_FullPipeline_20L)->MinTime(2.0);
