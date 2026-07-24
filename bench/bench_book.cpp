#include <benchmark/benchmark.h>
#include "book/order_book.h"
#include "bench_timing.h"
#include <vector>
#include <numeric>

// Timing methodology, tick-resolution measurements, and what p50/p99/p999
// mean here: see bench_timing.h. Current numbers live in bench/BASELINE.md,
// not here, to avoid two sources of truth going stale independently.

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

// ── BM_AddOrder ───────────────────────────────────────────────────────────────
// Each group: evict GROUP_SIZE refs (untimed), then add them all back as one
// timed batch. Book stays at POOL orders throughout. Same workload/setup as
// the original per-op harness — only the timing granularity changed.
static void BM_AddOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts = POOL + 1;
    uint64_t group_idx = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL;

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.delete_order(ts++, ref);  // evict (untimed)
        }

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.add_order(ts++, ref, 'B', 100, price_for(ref));  // timed
        }
        auto t1 = Clock::now();

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_AddOrder)->MinTime(2.0);

// ── BM_DeleteOrder ────────────────────────────────────────────────────────────
// Each group: delete GROUP_SIZE refs as one timed batch, then re-add them all
// (untimed) to restore the book.
static void BM_DeleteOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts = POOL + 1;
    uint64_t group_idx = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL;

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.delete_order(ts++, ref);  // timed
        }
        auto t1 = Clock::now();

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.add_order(ts++, ref, 'B', 100, price_for(ref));  // replenish (untimed)
        }

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_DeleteOrder)->MinTime(2.0);

// ── BM_ReplaceOrder ───────────────────────────────────────────────────────────
// Each group: replace GROUP_SIZE live refs with fresh refs as one timed batch.
// live_refs tracks current refs so the book stays valid across groups.
static void BM_ReplaceOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<uint64_t> live_refs(POOL);
    std::iota(live_refs.begin(), live_refs.end(), uint64_t{1});

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts          = POOL + 1;
    uint64_t new_ref_seq = POOL + 1;
    uint64_t group_idx   = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL;

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t idx     = (base + k) % POOL;
            uint64_t old_ref = live_refs[idx];
            uint64_t new_ref = new_ref_seq++;
            uint32_t price   = price_for(idx + 1);
            book.replace_order(ts++, old_ref, new_ref, 100, price);  // timed
            live_refs[idx] = new_ref;
        }
        auto t1 = Clock::now();

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_ReplaceOrder)->MinTime(2.0);

// ── BM_ExecuteOrder ───────────────────────────────────────────────────────────
// Each group: execute GROUP_SIZE refs for 50 shares each as one timed batch,
// then delete+re-add all of them (untimed) to restore 100 shares.
static void BM_ExecuteOrder(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book(book);

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts = POOL + 1;
    uint64_t group_idx = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL;

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.execute_order(ts++, ref, 50);  // timed (partial fill)
        }
        auto t1 = Clock::now();

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL) + 1;
            book.delete_order(ts++, ref);
            book.add_order(ts++, ref, 'B', 100, price_for(ref));  // restore (untimed)
        }

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
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

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts = POOL_20 + 1;
    uint64_t group_idx = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL_20;

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL_20) + 1;
            book.delete_order(ts++, ref);
        }

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL_20) + 1;
            book.add_order(ts++, ref, 'B', 100, price_for_20(ref));
        }
        auto t1 = Clock::now();

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_AddOrder_20L)->MinTime(2.0);

static void BM_DeleteOrder_20L(benchmark::State& state) {
    OrderBook book("TEST");
    fill_book_20(book);

    std::vector<double> group_means_ns;
    group_means_ns.reserve(1'000'000);

    uint64_t ts = POOL_20 + 1;
    uint64_t group_idx = 0;

    for (auto _ : state) {
        uint64_t base = (group_idx * GROUP_SIZE) % POOL_20;

        auto t0 = Clock::now();
        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL_20) + 1;
            book.delete_order(ts++, ref);
        }
        auto t1 = Clock::now();

        for (uint32_t k = 0; k < GROUP_SIZE; ++k) {
            uint64_t ref = ((base + k) % POOL_20) + 1;
            book.add_order(ts++, ref, 'B', 100, price_for_20(ref));
        }

        if (group_idx >= WARMUP_GROUPS)
            group_means_ns.push_back(elapsed_ns(t0, t1) / GROUP_SIZE);
        ++group_idx;
    }

    report_grouped_percentiles(state, group_means_ns);
}
BENCHMARK(BM_DeleteOrder_20L)->MinTime(2.0);
