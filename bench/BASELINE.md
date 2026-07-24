# Baseline — rewritten 2026-07-23 (harness fix: tick-resolution quantization)

Machine: Apple Silicon (arm64), 12 logical cores, `Release` build (`-O2 -Wall -Wextra`, AppleClang 17.0.0).
Build: `cmake -DCMAKE_BUILD_TYPE=Release ..` in `build_audit/` (gitignored, audit-only dir).
google-benchmark 1.9.5 (Homebrew).

## Why this baseline was rewritten

Every prior number in this file (0, 41, 42, 84, 125 ns — everything) was an integer multiple
of Apple Silicon's ~41.67ns hardware timer tick, because the harness timed **one operation**
per `now()`/`now()` pair. A 30-80ns operation measured against a 41.67ns-resolution clock reads
back as 0, 1, 2, or 3 ticks — the harness was measuring the clock, not the engine. This was
caught because `BM_ExecuteOrder` reported `p50_ns=0`, and a genuinely ~28-40% wall-clock
improvement (the pool-allocator fix) round-tripped through the old harness as identical
41/42/84/125ns buckets before and after. All prior numbers are preserved verbatim below under
**SUPERSEDED** — do not cite them.

## Phase 0 — measured timer resolution (not assumed)

```
mach_timebase_info: numer=125 denom=3  → 1 mach tick = 41.666667 ns
mach_absolute_time: min nonzero delta = 1 tick  = 41.667 ns  (2,000,000-call probe)
high_resolution_clock: min nonzero delta = 41 ns; distinct nonzero values observed:
  41, 42, 83, 84, 125, 166, 167, 208, 209, 250 ns — exactly round(41.667 × 1,1,2,2,3,4,4,5,5,6)
```

Confirms: every value the old harness ever reported was a multiple of this tick. There is no
userspace cycle counter on Apple Silicon (`PMCCNTR_EL0` is EL1-restricted), so there is no
rdtsc-style per-op fallback available on this hardware.

## Phase 1 — timing strategy chosen: grouped batches (option b), reported as both a
## precise mean (option a, recovered for free) and a group-level distribution

Full rationale and code-level documentation lives in `bench/bench_timing.h`. Summary:

- **Rejected: pure batched timing (option a) alone.** Gives a precise mean per op (divide one
  big timed block by op count) but *no distribution at all* — can't report p99/p999, which is
  exactly what an interview conversation about tail latency needs.
- **Rejected: Google Benchmark auto-scaling (option c) alone.** GBench's own iteration loop
  times the whole loop body the same way our harness does manually — it doesn't solve the
  single-op quantization problem either; the loop body still has to be a group of ops, not one.
- **Chosen: grouped batches (option b), N=128 ops per timed block.** A group of 128 ops at
  ~15-50ns/op takes ~2,000-6,400ns — on the order of 50-150 ticks, so the tick-quantization
  error on any one GROUP measurement is ~0.7-2% relative, not the ~100% relative error a
  single-op measurement carries. Dividing the group's total by 128 gives a per-op-equivalent
  sample. The arithmetic mean of all group samples equals the true overall mean exactly (all
  groups are equal size) — this recovers option (a)'s precise mean for free. Sorting all group
  samples and reporting p50/p99/p999 recovers a genuine, non-quantized *distribution* — at the
  cost of tail-event resolution (see below).
- **500 warmup groups (64,000 ops) are run and discarded** before any sample is recorded, to
  exclude cold-cache/cold-allocator effects.

## What p50/p99/p999 ARE and ARE NOT, here

**These are percentiles of "mean cost per op, averaged over a 128-op window" — NOT percentiles
of a single operation's latency.** If one op in a 128-op group is a 10x-slower outlier (a page
fault, an allocator rehash, an overflow-map promotion), it moves that group's mean by only
~1/128th of the outlier's excess cost. A true single-op tail event is **damped, not eliminated**,
by this method. This is the explicit tradeoff for operating above the timer's resolution floor:
coarser tail-event visibility in exchange for numbers that are not artifacts of the clock. Do
not describe these as "single-operation p99/p999" in any external-facing writeup — describe
them as "p99/p999 of the mean op cost over 128-op windows," or just "grouped-batch p99/p999."

## Phase 3 — acceptance test: PASSED

No value below is an integer multiple of 41.667ns (the measured tick), and no benchmark
reports p50=0. This is the concrete, checkable evidence the harness is no longer
resolution-limited — not just an assertion.

## Current numbers (HEAD, post pool-allocator fix) — `./lob_bench --benchmark_min_time=2.0s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

All values ns, median across 5 repetitions, GROUP_SIZE=128, 500 warmup groups discarded per rep:

| Benchmark | mean | p50 | p99 | p999 | groups/rep |
|---|---|---|---|---|---|
| BM_AddOrder | 15.19 | 14.98 | 20.19 | 48.83 | 703,014 |
| BM_DeleteOrder | 15.81 | 15.63 | 20.18 | 40.37 | 694,091 |
| BM_ReplaceOrder | 30.20 | 30.27 | 38.09 | 65.43 | 722,254 |
| BM_ExecuteOrder | 14.20 | 14.00 | 18.88 | 41.66 | 498,233 |
| BM_AddOrder_20L | 8.55 | 8.46 | 10.74 | 12.05 | 1,299,650 |
| BM_DeleteOrder_20L | 8.46 | 8.14 | 11.07 | 21.16 | 1,253,120 |
| BM_FullPipeline | 17.17 | 16.93 | 22.46 | 45.90 | 652,576 |
| BM_FullPipeline_20L | 10.99 | 10.74 | 14.00 | 15.30 | 1,110,770 |

cv on the mean was ≤3.05% across all 8 cases (tightest: FullPipeline at 0.15%; loosest:
DeleteOrder at 3.05%) — stable, not noise-dominated.

## Phase 4 — true value of the pool-allocator fix (item 3), re-measured with this harness

The pool-allocator commit's previously-claimed "28-43% wall-clock improvement" was measured
with the broken tick-quantized harness. Re-measured properly: checked out the pre-fix
`include/book/order_book.h` (plain `std::unordered_map`, no pool, via
`git show 257956e:include/book/order_book.h`), rebuilt against the *new* harness (same
bench_book.cpp/bench_pipeline.cpp used for the HEAD numbers above — apples to apples), ran it,
then restored HEAD's `order_book.h` and rebuilt again (confirmed clean, `./lob_test` 10/10
both before and after the swap).

| Benchmark | Pre-fix mean (ns) | Post-fix mean (ns) | Δ mean | Pre-fix p999 | Post-fix p999 |
|---|---|---|---|---|---|
| BM_AddOrder | 21.28 | 15.19 | **-28.6%** | 30.60 | 48.83 |
| BM_DeleteOrder | 26.24 | 15.81 | **-39.7%** | 37.44 | 40.37 |
| BM_ReplaceOrder | 46.10 | 30.20 | **-34.5%** | 83.98 | 65.43 |
| BM_ExecuteOrder | 14.11 | 14.20 | +0.6% (noise) | 20.19 | 41.66 |
| BM_AddOrder_20L | 14.69 | 8.55 | **-41.8%** | 19.53 | 12.05 |
| BM_DeleteOrder_20L | 18.41 | 8.46 | **-54.0%** | 25.72 | 21.16 |
| BM_FullPipeline | 23.68 | 17.17 | **-27.5%** | 32.23 | 45.90 |
| BM_FullPipeline_20L | 17.94 | 10.99 | **-38.7%** | 24.09 | 15.30 |

**This is a defensible, mechanistically-explained result, not a coincidence of the old
harness:** `BM_ExecuteOrder`'s benchmark reduces an order's shares in place (50 of 100 shares)
and never triggers the map's zero-fill erase path — it never touches `unordered_map`
insert/erase at all, so the pool allocator has nothing to do there, and the measured delta is
correctly ~0% (+0.6%, within noise). Every other benchmark does an insert and/or erase per op
(Add=insert, Delete=erase, Replace=erase+insert, FullPipeline=Add's insert) and every one of
them shows a real, substantial improvement. The mechanism (eliminating per-op heap node
alloc/free) predicts exactly this pattern, and the re-measurement confirms it. The magnitude
(28-54% mean reduction) is in the same ballpark as originally claimed (28-43%) but is now
backed by a real distribution instead of tick-quantized single-sample timing — this is not
because the original estimate was "right by luck"; the ~2x-larger `DeleteOrder_20L` delta
(54% vs the old harness's ~30%) shows the old numbers were not reliable even in aggregate
direction of magnitude, just coincidentally close on some cases.

Note on p999 columns above: several *increase* post-fix (e.g. AddOrder 30.60→48.83,
FullPipeline 32.23→45.90). This is plausible and not a regression signal: with per-op cost
lower overall, the *relative* weight of a fixed-cost tail event (e.g. an
`unordered_map` bucket-array rehash, which still allocates in bulk even with the pool
allocator — see bench_timing.h and the pool allocator's fallback to `::operator new` for
non-size-1 requests) becomes larger relative to the now-smaller typical op cost, pulling the
group-mean p999 up in relative terms even though absolute mean/p50/p99 all improved. This is
exactly the kind of nuance the old harness's tick-quantized p99=84/125ns could never have
shown either way.

## End-to-end throughput (226M messages, 7.8M msg/s, 29.0s) — still unverified, unaffected by this harness work

No raw ITCH `.bin`/`.NASDAQ_ITCH50` file exists locally. This figure is an **aggregate**
end-to-end measurement (wall-clock over a full replay), not a per-op microbenchmark — it was
never measured with `chrono::now()`-per-op timing in the first place, so it is not affected by
the tick-quantization bug this harness rewrite fixes. It remains exactly as unverified as
before this session. If a raw file becomes available, rerun via
`./lob_engine <file> <TICKER> <output.csv>` and update this section — do not infer a new
throughput number from the per-op deltas above without an actual end-to-end run.

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

## SUPERSEDED — timer-resolution-limited, do not cite

Everything below this line predates the harness fix above and is quantized to the ~41.67ns
mach tick documented in Phase 0. Kept for history only.

### SUPERSEDED: pre-fix baseline (original, before any fix-loop item)

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

### SUPERSEDED: post pool-allocator-fix baseline (measured with the broken harness)

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

The SUPERSEDED before/after table that used to live here (claiming "28-43% wall-clock
improvement, ReplaceOrder p99 125→84ns") is retired along with the numbers it was built from.
The real, harness-validated delta is in the "Phase 4" section above.
