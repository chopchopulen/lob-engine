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

## p999 investigation (2026-07-23) — the "relative weight" explanation above was wrong

The note originally here claimed the post-fix p999 increases (AddOrder 30.60→48.83,
FullPipeline 32.23→45.90, ExecuteOrder 20.19→41.66) were a *relative-weighting* artifact: a
fixed-cost tail event becoming a larger fraction of a now-smaller typical op cost. **That
explanation does not survive scrutiny and is retracted.** A relative-weighting argument
predicts the tail event's absolute nanosecond cost is unchanged — but these are absolute
increases in nanoseconds, not just proportionally larger shares of a smaller mean. It also
cannot explain ExecuteOrder, whose mean did not move at all (14.11→14.20ns, i.e., no "smaller
typical cost" occurred) while its p999 more than doubled.

**Hypothesis tested instead: p999 regressions track net pool-allocator growth** (AddOrder and
FullPipeline evict-then-refill each group and were suspected to grow the pool monotonically,
forcing a rare bulk chunk-allocation spike; DeleteOrder/ReplaceOrder don't grow the pool and
were the two that didn't regress).

**Step 1 — read the allocator (`include/book/order_book.h`, `PoolAllocator<T>`).** It has no
chunking or bulk-growth policy at all: `allocate(1)` pops one node off an intrusive free-list,
or on a miss calls `::operator new(sizeof(T))` for exactly **one** node — never a batch/chunk.
Only non-size-1 requests (the `unordered_map` bucket array, not order nodes) go through
`::operator new(n * sizeof(T))`, and `orders_.reserve(1<<20)` in the constructor pre-sizes
that bucket array once, up front, well above the benchmark pool sizes (10,000 / 1,000) — no
rehash occurs during any of these runs. There is no pre-reservation of free-list nodes; the
free-list only fills as orders are deleted.

**Step 2 — instrumented the allocator directly** (temporary counters on the free-list-miss
path, `single_new_calls()`, not shipped — reverted after this investigation) and confirmed via
code reading that `BM_AddOrder`/`BM_FullPipeline` evict exactly `GROUP_SIZE` orders
immediately before re-adding the same `GROUP_SIZE` refs at the same prices each group — net
pool size is constant after warmup, by construction, not just empirically.

**Step 3 — direct test.** Ran `BM_AddOrder` and `BM_FullPipeline` for 16 total post-warmup
repetitions (8 reps × 2 benchmarks) with the instrumented build:
`pool_new_calls_post_warmup = 0` in **every single repetition, no exceptions**. The free-list
never misses after warmup. The hypothesis is **refuted** — there is no chunk-growth event for
this instrumentation to have measured a spike from.

**What's actually going on:** with pool growth ruled out, all 5 primary benchmarks (not just
the ones flagged as "regressed") were re-run 8 times each in one process
(`--benchmark_repetitions=8 --benchmark_report_aggregates_only=false`,
`--benchmark_min_time=1.5s`) to characterize repetition-to-repetition variance:

| Benchmark | mean cv | p50 cv | p99 cv | p999 cv | p999 range across 8 reps |
|---|---|---|---|---|---|
| BM_AddOrder | 0.70% | 0.74% | 27.15% | 18.72% | 26.0 – 52.7 ns |
| BM_DeleteOrder | 1.19% | 0.72% | 5.01% | 32.12% | 22.1 – 44.9 ns |
| BM_ReplaceOrder | 1.12% | 0.38% | 7.29% | 32.82% | 42.0 – 89.5 ns |
| BM_ExecuteOrder | 0.28% | 0.00% | 4.98% | 28.23% | 19.2 – 37.1 ns |
| BM_FullPipeline | 0.57% | 0.98% | 5.52% | 42.83% | 23.8 – 54.0 ns |

p999's cv is 18–43% for every benchmark — **including `BM_DeleteOrder` and
`BM_ReplaceOrder`**, the two the original Phase 4 table called "flat" and "improved." Both
show ~2x rep-to-rep swings in p999, the same relative magnitude as `BM_AddOrder`,
`BM_ExecuteOrder`, and `BM_FullPipeline`. Mean and p50 are stable (cv ≤1.2%) across all five;
p999 alone is not.

**Conclusion:** the Phase 4 table compared a single pre-fix p999 sample against a single
post-fix p999 sample for each benchmark. Given p999's demonstrated 18–43% cv on *identical,
unchanged code*, a single-sample before/after diff of p999 is not a reliable signal of a
directional change — it is well within the noise band this statistic exhibits regardless of
the allocator. The apparent "regression" in three benchmarks and "flat"/"improvement" in the
other two is consistent with which side of that noise distribution each single sample happened
to land on, not with a real allocator-driven mechanism. p999 in this harness measures rare,
low-sample-count tail events (recall from Phase 1: it's the percentile of ~500-1,300k *group*
means, so the p999 estimate itself rests on very few extreme samples) most plausibly explained
by host-level jitter (scheduler preemption, page faults, thermal/frequency scaling) landing
inside a 128-op window — not a deterministic property of the code under test.

**ExecuteOrder anomaly, resolved:** its mean/p50 are the most stable of all five benchmarks
(cv 0.28% / 0.00%) — expected, since it never calls `orders_.insert`/`erase` and the pool
allocator has no relevant code path to affect. Its p999 swung 19.2–37.1ns (cv 28%) across 8
reps in the same run, in line with the other four benchmarks' p999 noise level. This is fully
explained by the same measurement-variance mechanism above; no allocator-related or other
mechanistic cause is indicated, and none is claimed.

**No engine fix applied.** Step 5 of this investigation ("propose a fix, measure
before/after") is void: the hypothesis it was conditioned on ("if confirmed, propose a fix")
did not confirm. Changing the pool allocator's chunk/reservation strategy would not address a
mechanism that isn't there, and doing so anyway risked introducing a real change on the
strength of a false premise. `PoolAllocator<T>` is unchanged from the version verified in the
item-3 fix loop.

**p999 on this machine reflects OS scheduling, not engine behavior.** This setup runs on an
unpinned, shared host (no core pinning, no isolation from the rest of the OS scheduler) — the
tail events driving p999's 18–43%+ cv are plausibly ordinary scheduler preemption, page
faults, or frequency-scaling transitions landing inside a 128-op window, not a property of the
code under test. Stable tail-latency measurement (a p999 trustworthy enough to diff
single-sample before/after) would require a pinned core on an otherwise-quiet host, which this
setup does not provide. Treat p999 here as "this machine's noise floor," not as an engine
characteristic.

## p50/p99 stability check (2026-07-23) — which percentiles are safe to cite

Prompted by publishing a p99 figure in README.md: is p99 as noise-dominated as p999, or closer
to p50/mean? Re-ran all 8 benchmarks, 8 repetitions each, same command as above
(`--benchmark_repetitions=8 --benchmark_report_aggregates_only=true`,
`--benchmark_min_time=1.5s`):

| Benchmark | mean cv | p50 cv | p99 cv | p999 cv |
|---|---|---|---|---|
| BM_AddOrder | 7.32% | 1.10% | 57.16% | 49.49% |
| BM_DeleteOrder | 11.80% | 1.44% | 96.93% | 108.08% |
| BM_ReplaceOrder | 0.98% | 0.50% | 1.64% | 36.58% |
| BM_ExecuteOrder | 4.49% | 1.17% | 30.09% | 79.24% |
| BM_AddOrder_20L | 7.40% | 2.81% | 34.20% | 93.80% |
| BM_DeleteOrder_20L | 2.03% | 0.00% | 27.99% | 39.60% |
| BM_FullPipeline | 0.50% | 0.00% | 7.36% | 43.34% |
| BM_FullPipeline_20L | 0.50% | 0.00% | 2.84% | 34.87% |

**p50 is stable and safe to cite single-sample: cv ≤2.81% on every benchmark**, consistent
with the earlier finding. Mean is also generally low (≤11.80%, mostly <8%) but noisier than
p50 on this run — this run itself shows the mean is not perfectly immune to the same host
jitter, just far less exposed to it than p99/p999.

**p99 is noise-dominated on 6 of 8 benchmarks (cv 27–97%)** — essentially as unreliable as
p999. Only `BM_ReplaceOrder` (1.64%) and `BM_FullPipeline_20L` (2.84%) show p99 stable enough
to trust single-sample; `BM_FullPipeline` — the benchmark README.md currently cites a p99
figure from — sits at 7.36% cv, better than most but still an order of magnitude noisier than
its own p50 (0.00% this run). `BM_DeleteOrder`'s p99 cv of 96.93% means the statistic is
essentially meaningless as reported: its own repetition-to-repetition spread is comparable to
its value.

**Recommendation:** do not cite p99 as a stable single-sample figure for any of these
benchmarks, including `BM_FullPipeline`. Only mean and p50 are currently defensible as
single-sample citable statistics; p99 and p999 require the same treatment — reported only as
a multi-repetition range, or not cited at all, on this unpinned host.

## End-to-end throughput (226M messages, 7.8M msg/s, 29.0s) — VERDICT: RETIRED, replaced below (2026-07-25)

A raw main-feed file (2019-12-30, `data/raw/12302019.NASDAQ_ITCH50`, 8.25GB decompressed)
became available this session, making this claim testable for the first time. Measured on an
Apple M3 Pro (`arm64`, this repo's dev machine).

**The claim as originally stated cannot be reproduced, and is retired, not corrected to a new
single number — it conflates two different things this codebase's architecture cannot do in
one measurement:**

- "226M messages" implies whole-file scope (every message, every instrument).
- "7.8M msg/s ... full pipeline" implies parsing AND book reconstruction AND feature
  computation for all of them.
- `OrderBook` is one instrument's book. `main.cpp` only ever instantiates one or two
  `OrderBook` objects (single- or dual-ticker mode) — there is no code path, past or present,
  that reconstructs order books for every instrument in a file simultaneously. Doing so would
  require one book per `stock_locate` (thousands, for a main-feed file) — a fundamentally
  different, much larger feature than anything in this repo. **No measurement of "226M
  messages through full book reconstruction" is possible with this codebase**, so the original
  claim cannot be verified, corrected, or attributed to a real run of this code as stated.

**What was actually measured instead, as two separate, honestly-scoped numbers:**

1. **Whole-file parse throughput, unfiltered, no book/feature reconstruction** (directly
   comparable in methodology to the BX `bench_parser` figure below, now on real main-feed
   data): `LOB_BENCH_ITCH_FILE=data/raw/12302019.NASDAQ_ITCH50 ./lob_bench
   --benchmark_filter='BM_ParseFile' --benchmark_repetitions=1` →
   **263.24M messages, 14.0–14.3s wall (2 runs), 18.4–18.8M msg/s.** Message count is stable
   run-to-run (deterministic parse); wall time varies by ~2% across runs. This is the closest
   honest analog to the original claim's "whole file, hundreds of millions of messages" shape —
   and it is **2.4x faster** than the originally-claimed 7.8M msg/s, but it is parse-only, not
   full pipeline, so it does not confirm the original number even loosely; it measures a
   different thing that happens to be in the same ballpark of message count.
2. **Single-ticker full pipeline** (parse + filter + `OrderBook` reconstruction +
   `FeatureEngine` on AAPL, same file): `./lob_engine data/raw/12302019.NASDAQ_ITCH50 AAPL
   /tmp/out.csv` → 1,512,179 messages matched the AAPL filter, 13.54s wall, "0.112M msg/s" by
   `main.cpp`'s own printed metric (`msg_count / elapsed_s`). **This number is not comparable
   to anything above or to the original claim** — the 13.54s is dominated by sequentially
   scanning the full 8.25GB file byte-by-byte to find AAPL's ~0.57% of messages, not by
   per-message book-update cost (which is already covered, correctly, by `BM_AddOrder`/
   `BM_ExecuteOrder`/etc. above at 8-15ns/op). Reported for completeness, not as a throughput
   claim.

**Verdict: RETIRED.** Neither number above is "the" 226M/7.8M figure, corrected or otherwise —
that combined figure doesn't correspond to a measurement this codebase's architecture can
produce. Do not cite 226M/7.8M/29.0s going forward. The two numbers above are the closest real,
reproducible substitutes, kept walled off from each other and from the BX `bench_parser` figure
(different data, different scope, do not average or compare them as if validating one another).

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

## Parser throughput (`bench/bench_parser.cpp`, added 2026-07-24)

**Why this exists:** every other benchmark in this file exercises `OrderBook`/`FeatureEngine`
directly — none of them ever call `itch_parser.cpp`. When the `stock_locate` refactor changed
the parser (`PROJECT_STATUS.md`), rerunning this file's benchmarks against it was a null test
by construction. This closes that specific coverage gap — it measures the parser in isolation
on real data. It is **not** a component-level breakdown of the 226M-msg/7.8M-msg/s end-to-end
claim (see below for exactly why not) — that figure remains separately, fully unverified.

**Measured on:** Nasdaq BX ITCH sample, 2019-07-30
(`https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH/20190730.BX_ITCH_50.gz`, ~373MB compressed /
~837MB decompressed). Not committed to the repo (real ITCH files are hundreds of MB, same
reason `data/*.bin` is gitignored) — set `LOB_BENCH_ITCH_FILE` to a local copy to reproduce.
Timing discipline adapted from `bench_timing.h`'s grouped-batch approach — see
`bench/bench_parser.cpp`'s header comment for why literal `GROUP_SIZE=128` batching doesn't
apply here (one full-file parse is already tens of thousands of ticks, far above the
tick-resolution floor that motivated grouping for single-op `OrderBook` benchmarks). Unfiltered
scan (every message dispatched to a callback, matching "total messages" exercising every
parsing branch) — a realistic filtered single-ticker pass is faster than this, not slower, since
most messages take the cheap early-exit skip path instead of full decode + dispatch.

```bash
LOB_BENCH_ITCH_FILE=/path/to/20190730.BX_ITCH_50 \
  ./lob_bench --benchmark_filter='BM_ParseFile' --benchmark_min_time=1.0s \
              --benchmark_repetitions=8 --benchmark_report_aggregates_only=false
```

| Metric | mean | p50 (median of reps) | cv |
|---|---|---|---|
| Wall time per full-file parse | 1515 ms | 1512 ms | 1.37% |
| Throughput | 15.72M msgs/sec | 15.75M msgs/sec | 1.35% |
| Per-message cost | 63.6 ns | 63.5 ns | 1.37% |

23,821,600 messages dispatched per rep (every `'A'`/`'F'`/`'D'`/`'U'`/`'E'`/`'C'`/`'X'` message
in the file — message types without a registered callback, e.g. `'R'` Stock Directory, System
Event, quoting/auction messages, are read and skipped but not counted in this total). cv
≤1.37% across 8 repetitions — stable, unlike the `OrderBook` microbenchmarks' p99/p999 (see the
p50/p99 stability section above); this workload's dominant cost is CPU-bound field decode +
callback dispatch on an OS-page-cache-warm file, not the same kind of rare-tail-event noise.

**This figure does not support, validate, or replace the 226M-msg/7.8M-msg/s end-to-end
claim, and should not be read as doing so.** They are two different measurements of two
different things on two different data sources:

| | Parser throughput (this section) | End-to-end throughput claim |
|---|---|---|
| What's measured | Raw message parsing only — `itch_parser.cpp` scanning + field decode + callback dispatch | Full pipeline — parse + `OrderBook` reconstruction + `FeatureEngine` computation |
| Data | Nasdaq **BX** (a small regional venue), 2019-07-30, ~28.7M messages | Nasdaq **main feed** (implied by "226M messages"), venue/date unspecified in the original claim |
| Status | Measured this session, reproducible via the command above | Still separately unverified — no raw main-feed file available locally (see `PROJECT_STATUS.md`) |

BX carries a small fraction of main-feed volume and message-type mix (see
`PROJECT_STATUS.md` Task 1 for venue size comparisons); parser throughput on a low-volume
venue sample is not a proxy for full-pipeline throughput on main-feed volume. The two numbers
happening to be within 2x of each other (15.72M vs 7.8M) is not evidence for either one — it
is not a validation, and no claim of consistency between them should be drawn from it. The
226M/7.8M end-to-end figure remains exactly as unverified as before this benchmark existed,
pending an actual main-feed run once a raw file is available.
