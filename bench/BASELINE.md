# Baseline — captured 2026-07-23

Machine: Apple Silicon (arm64), 12 logical cores, `Release` build (`-O2 -Wall -Wextra`, AppleClang 17.0.0).
Build: `cmake -DCMAKE_BUILD_TYPE=Release ..` in `build_audit/` (gitignored, audit-only dir).
google-benchmark 1.9.5 (Homebrew).

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

All 9 tests passed.
```

## Microbenchmarks — `./lob_bench --benchmark_min_time=2.0s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

Verbatim (median row per case; full output has mean/median/stddev/cv):

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

Stddev/cv were low (≤8.5% cv on wall time, most under 2%) — runs are stable, not noise-dominated.

`p50_ns=41-42`, `p99_ns=42-125` across cases roughly line up with the README's headline "median ~41ns, p99 84ns" — the resume numbers are reproducible from these microbenchmarks on this machine, **for the in-flat-array hot path**. `BM_ReplaceOrder` shows a fatter p99 tail (125ns, cv 25.9% on that counter) worth flagging to perf-analyst.

## End-to-end throughput (226M messages, 7.8M msg/s, 29.0s) — NOT reproduced

No raw ITCH `.bin`/`.NASDAQ_ITCH50` file exists locally (`data/` only has derived CSVs). `download_itch.sh` pulls 5–12GB/day files from Nasdaq's public archive and none are cached. This number in the README **cannot currently be verified on this machine** — flagged for the architect/user rather than assumed correct. If a raw file is supplied, rerun via `./lob_engine <file> <TICKER> <output.csv>` and update this section.

## How to reproduce

```bash
cd lob-engine
rm -rf build_audit && mkdir build_audit && cd build_audit
cmake -DCMAKE_BUILD_TYPE=Release .. -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build . -j 8
./lob_test
./lob_bench --benchmark_min_time=2.0s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```
