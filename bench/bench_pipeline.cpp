#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include "features/feature_engine.h"
#include "bench_timing.h"
#include <vector>

// Timing methodology, tick-resolution measurements, and what p50/p99/p999
// mean here: see bench_timing.h. Current numbers live in bench/BASELINE.md,
// not here, to avoid two sources of truth going stale independently.

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
// Each group:
//   1. evict GROUP_SIZE bid refs (untimed — keeps depth stable)
//   2. add them all back + run features.on_book_update for each, as one
//      timed batch
//
// This is the dominant message type (~60% of ITCH traffic) and the exact
// hot path that runs 226M times on a full trading day (that end-to-end
// throughput claim is separately unverified — see bench/BASELINE.md).
static void BM_FullPipeline(benchmark::State& state) {
    OrderBook     book("TEST");
    FeatureEngine features;
    fill_book_pipeline(book);

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t group_idx = 0;
    // Start timestamp at 9:30 AM in nanoseconds (Nasdaq timestamps are
    // nanoseconds since midnight; 9:30 = 34,200 seconds)
    uint64_t ts = 34'200'000'000'000ULL;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % BID_COUNT;

        // Only the bid side is rotated; asks remain static to isolate bid-add latency.
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % BID_COUNT) + 1;
            book.delete_order(ts++, ref);  // evict (untimed)
        }

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % BID_COUNT) + 1;
            book.add_order(ts, ref, 'B', 100, bid_price(ref));
            features.on_book_update(book, ts);  // add_order and on_book_update share ts — same event
            ts++;
        }
        auto t1 = Clock::now();

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
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

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t group_idx = 0;
    uint64_t ts = 34'200'000'000'000ULL;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % BID_COUNT_20;

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % BID_COUNT_20) + 1;
            book.delete_order(ts++, ref);
        }

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % BID_COUNT_20) + 1;
            book.add_order(ts, ref, 'B', 100, bid_price_20(ref));
            features.on_book_update(book, ts);
            ts++;
        }
        auto t1 = Clock::now();

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_FullPipeline_20L)->MinTime(2.0);
