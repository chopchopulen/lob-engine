# Baseline — updated 2026-07-23 (post audit-fix-loop)

Machine: Apple Silicon (arm64), 12 logical cores, `Release` build (`-O2 -Wall -Wextra`, AppleClang 17.0.0).
Build: `cmake -DCMAKE_BUILD_TYPE=Release ..` in `build_audit/` (gitignored, audit-only dir).
google-benchmark 1.9.5 (Homebrew).

This baseline reflects the engine **after** fix-loop items 1, 2, 4, 3 from `audit/FINDINGS.md`
(ITCH 'X' Cancel handling, 'C' Executed-With-Price handling, ticker truncation fix, and a
pool-allocator for `OrderBook::orders_`). The original pre-fix baseline is preserved below
under "Pre-fix baseline" for reference/history.

## Unit tests — `./lob_test`

```
PASS test_basic_top_of_book
PASS test_bid_levels_ordering
PASS test_ask_levels_ordering
PASS test_multiple_orders_same_level
PASS test_delete_removes_level
PASS test_overflow_and_promotion_bids
PASS test_overflow_and_promotion_asks
PASS test_execute_order
PASS test_replace_order
PASS test_cancel_order

All 10 tests passed.
```

(9→10: `test_cancel_order` added alongside the 'X' Order Cancel fix.)

## Microbenchmarks — `./lob_bench --benchmark_min_time=2.0s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

Verbatim (median row per case):

```
Benchmark                                          Time             CPU   Iterations UserCounters...
BM_AddOrder/min_time:2.000_median               44.1 ns         44.1 ns            5 items_per_second=22.6953M/s p50_ns=41 p99_ns=42
BM_DeleteOrder/min_time:2.000_median            43.8 ns         43.8 ns            5 items_per_second=22.8087M/s p50_ns=41 p99_ns=42
BM_ReplaceOrder/min_time:2.000_median           52.9 ns         52.9 ns            5 items_per_second=18.909M/s  p50_ns=42 p99_ns=84
BM_ExecuteOrder/min_time:2.000_median           58.1 ns         58.1 ns            5 items_per_second=17.2062M/s p50_ns=0  p99_ns=42
BM_AddOrder_20L/min_time:2.000_median           34.9 ns         34.9 ns            5 items_per_second=28.6937M/s p50_ns=0  p99_ns=42
BM_DeleteOrder_20L/min_time:2.000_median        35.1 ns         35.1 ns            5 items_per_second=28.5145M/s p50_ns=0  p99_ns=42
BM_FullPipeline/min_time:2.000_median           49.3 ns         49.3 ns            5 items_per_second=20.272M/s  p50_ns=41 p99_ns=42
BM_FullPipeline_20L/min_time:2.000_median       37.5 ns         37.5 ns            5 items_per_second=26.699M/s  p50_ns=0  p99_ns=42
```

cv ≤2.6% on every case (down from ≤8.5% pre-fix) — allocator-induced tail variance is gone.

## Before / after (fix-loop items 1, 2, 4, 3)

| Benchmark | Before (wall median) | After (wall median) | Δ | p99 before → after |
|---|---|---|---|---|
| BM_AddOrder | 67.9 ns | 44.1 ns | **-35%** | 84 → 42 ns |
| BM_DeleteOrder | 76.4 ns | 43.8 ns | **-43%** | 84 → 42 ns |
| BM_ReplaceOrder | 76.8 ns | 52.9 ns | **-31%** | 125 → 84 ns |
| BM_ExecuteOrder | 80.4 ns | 58.1 ns | **-28%** | 42 → 42 ns |
| BM_AddOrder_20L | 49.8 ns | 34.9 ns | -30% | 42 → 42 ns |
| BM_DeleteOrder_20L | 50.3 ns | 35.1 ns | -30% | 42 → 42 ns |
| BM_FullPipeline | 83.4 ns | 49.3 ns | **-41%** | 84 → 42 ns |
| BM_FullPipeline_20L | 60.3 ns | 37.5 ns | -38% | 84 → 42 ns |

Root cause of the win (item 3): `orders_` (`std::unordered_map<uint64_t,Order>`) had no pool/arena,
so every add/delete/replace/execute did a heap node alloc or free (replace did both). Added a
free-list `PoolAllocator<T>` in `include/book/order_book.h` that recycles freed nodes instead of
round-tripping through the general-purpose allocator, plus `orders_.reserve(1<<20)` at construction.
`BM_ReplaceOrder`'s fat p99 tail (125ns, cv 25.9%) — the specific anomaly flagged in
`audit/FINDINGS.md` finding #3 — is gone (84ns, cv ~2%), consistent with eliminating its
erase+insert double allocator round-trip. Independently reproduced twice by the adversarial
reviewer and once more here; no correctness regression (10/10 tests pass, confirmed by 3+
independent rebuild+test runs across this fix loop).

## End-to-end throughput (226M messages, 7.8M msg/s, 29.0s) — still NOT reproduced

Unchanged from the pre-fix baseline: no raw ITCH `.bin`/`.NASDAQ_ITCH50` file exists locally
(`data/` only has derived CSVs). This number in the README **still cannot be verified on this
machine**. It was neither confirmed nor refuted by this fix loop — the microbenchmark
improvements above make it *plausible* the real number is now higher than 7.8M msg/s (fewer
allocator round-trips per message), but this has not been measured end-to-end. If a raw file is
supplied, rerun via `./lob_engine <file> <TICKER> <output.csv>` and update this section — this
is the one number that still needs a real run before being claimed anywhere as changed.

## How to reproduce

```bash
cd lob-engine
rm -rf build_audit && mkdir build_audit && cd build_audit
cmake -DCMAKE_BUILD_TYPE=Release .. -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build . -j 8
./lob_test
./lob_bench --benchmark_min_time=2.0s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

---

## Pre-fix baseline (2026-07-23, before fix-loop items 1/2/4/3) — kept for history

```
PASS test_basic_top_of_book
PASS test_bid_levels_ordering
PASS test_ask_levels_ordering
PASS test_multiple_orders_same_level
PASS test_delete_removes_level
PASS test_overflow_and_promotion_bids
PASS test_overflow_and_promotion_asks
PASS test_execute_order
PASS test_replace_order

All 9 tests passed.
```

```
Benchmark                                          Time             CPU   Iterations UserCounters...
BM_AddOrder/min_time:2.000_median               67.9 ns         67.8 ns            5 items_per_second=14.7544M/s p50_ns=42 p99_ns=84
BM_DeleteOrder/min_time:2.000_median            76.4 ns         72.6 ns            5 items_per_second=13.7685M/s p50_ns=41 p99_ns=84
BM_ReplaceOrder/min_time:2.000_median           76.8 ns         73.4 ns            5 items_per_second=13.6147M/s p50_ns=42 p99_ns=125
BM_ExecuteOrder/min_time:2.000_median           80.4 ns         79.7 ns            5 items_per_second=12.5519M/s p50_ns=0  p99_ns=42
BM_AddOrder_20L/min_time:2.000_median           49.8 ns         49.7 ns            5 items_per_second=20.1015M/s p50_ns=41 p99_ns=42
BM_DeleteOrder_20L/min_time:2.000_median        50.3 ns         50.2 ns            5 items_per_second=19.9169M/s p50_ns=41 p99_ns=42
BM_FullPipeline/min_time:2.000_median           83.4 ns         78.6 ns            5 items_per_second=12.7294M/s p50_ns=42 p99_ns=84
BM_FullPipeline_20L/min_time:2.000_median       60.3 ns         58.8 ns            5 items_per_second=17.0192M/s p50_ns=41 p99_ns=84
```
