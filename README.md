> # ⚠️ READ [`docs/FINAL_NUMBERS.md`](docs/FINAL_NUMBERS.md) FIRST
>
> **[`docs/FINAL_NUMBERS.md`](docs/FINAL_NUMBERS.md) is the single source of truth** for every
> number in this project. It separates what is CLAIMABLE from what is RETIRED, lists what is
> still OPEN, and gives the reproduction pointer for each figure. If a number appears in this
> README and not there, treat this README as stale.
>
> Retired and no longer citable: all pre-harness-fix latency figures (tick-quantized by the
> ~41.667 ns Apple Silicon timer), the p99/p999 tail latencies (unmeasurable on this host),
> the "226M messages / 7.8M msg/s / 29.0 s" throughput claim (the parser had no ticker
> filtering at that commit), and **the entire original OFI research section** — it was computed
> on a book reconstruction corrupted by a `uint32_t` underflow bug, using a within-day 70/30
> split that crosses the intraday U-shape regime boundary, on data that was 58% undisclosed
> extended-hours. The corrected panel study replaces it below.
>
> Full defect list: [`audit/FINDINGS.md`](audit/FINDINGS.md).
> Benchmark methodology and cv tables: [`bench/BASELINE.md`](bench/BASELINE.md).

# lob-engine

A high-performance C++17 limit order book reconstruction engine for Nasdaq ITCH 5.0 binary feeds, with microstructure feature extraction and out-of-sample alpha research.

## Architecture

The pipeline is organized into four layers:

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1 — Feed Parser   (include/feed/, src/feed/)     │
│  Reads raw ITCH 5.0 binary; emits typed message structs │
├─────────────────────────────────────────────────────────┤
│  Layer 2 — Order Book    (include/book/, src/book/)     │
│  Maintains bid/ask price levels as flat sorted arrays   │
│  (replaced std::map; –11% pipeline latency at depth 20) │
├─────────────────────────────────────────────────────────┤
│  Layer 3 — Feature Engine (include/features/, src/)     │
│  Computes OFI, bid-ask spread, and trade imbalance      │
│  on 1-second intervals; writes CSV for downstream use   │
├─────────────────────────────────────────────────────────┤
│  Layer 4 — Analysis      (scripts/analysis.py)          │
│  OLS regression of normalized OFI against future price  │
│  changes; reports in-sample and out-of-sample R²        │
└─────────────────────────────────────────────────────────┘
```

## Dependencies

- C++17-compatible compiler (Apple Clang ≥ 12, GCC ≥ 9)
- GNU Make
- Python 3 with `pandas`, `numpy`, `statsmodels` (for `scripts/analysis.py`)

No third-party C++ libraries are required.

## Build

```bash
make          # produces ./lob_engine
make clean    # remove build/ and binary
```

The Makefile uses `-O2 -std=c++17` and auto-tracks header dependencies via `-MMD -MP`, so incremental builds only recompile what changed.

## Usage

```bash
./lob_engine <itch_file.bin> <TICKER> <output.csv>
```

**Example:**

```bash
./lob_engine data/raw/12302019.NASDAQ_ITCH50 AAPL data/features_AAPL.csv
```

The engine will print a progress summary on exit — real output, measured 2026-07-25 against a
real Nasdaq main-feed file (2019-12-30, 263.24M total messages, all instruments):

```
Done.
  Messages matched (this ticker): 1512179
  Trades:              60543
  Feature rows:        24729
  Active orders left:  0
  Elapsed (full run: locate resolution + parse + reconstruct + write): 27.8694s
  File size:           8251.41 MB
  Effective I/O rate:  296.074 MB/s (file is scanned twice per run; see note above)

Features written to: data/features_AAPL.csv
```

"Messages matched" is AAPL's own filtered count (~0.56% of the file's 268.74M total messages)
— every message type carries `stock_locate`, so filtering happens per-message, not via a
separate pass. `Elapsed` covers the whole run, including `parse_stock_directory()`'s own
full-file scan for ticker→locate resolution — an earlier version of this tool timed only the
main parse and undercounted real cost by ~2x; see
[`bench/BASELINE.md`](bench/BASELINE.md)'s "Real end-to-end measurement" for the fix and the
properly-paired throughput figures (real end-to-end throughput is ~19.0-19.7M msg/s once
correctly paired — "matched messages ÷ elapsed" is not a meaningful metric, since it divides a
filtered count by a full-file-scan time; that's why "Throughput" was replaced with MB/s above).
An **older version of this README previously cited a 226M-messages/7.8M-msg/s/29.0s figure for
this example — that figure was retired 2026-07-25 after a real file became available to test it.
It does not correspond to any measurement this codebase can produce**: `OrderBook` reconstructs
one instrument at a time (single- or dual-ticker mode), and no code path reconstructs order
books for every instrument in a file simultaneously — see `bench/BASELINE.md`'s "End-to-end
throughput" section for the full verdict and the two honestly-scoped replacement numbers.

Then run the regression analysis:

```bash
python3 scripts/analysis.py data/features_AAPL.csv
```

## Benchmark Results

Measured on Apple Silicon (arm64), single core, release build (`-O2`), via the grouped-batch
harness described in [Benchmark methodology](#benchmark-methodology) below. Full detail,
methodology, and reproduction steps: [`bench/BASELINE.md`](bench/BASELINE.md).

Headline figure — `BM_FullPipeline` (the dominant message type, ~60% of ITCH traffic):

| Metric | Value |
|---|---|
| Mean per-message latency | 17.17 ns |
| Median (p50) per-message latency | 16.93 ns |
| Whole-file parse throughput, unfiltered (real main-feed data, 2019-12-30) | 18.4-18.8M msg/s (263.24M messages) |

The previous "226M messages / 7.8M msg/s / 29.0s end-to-end" row has been retired (2026-07-25)
— it doesn't correspond to a measurement this codebase's single-instrument `OrderBook`
architecture can produce. See [`bench/BASELINE.md`](bench/BASELINE.md)'s "End-to-end
throughput" section for the full explanation and the real replacement numbers.

The flat sorted-array book representation eliminates pointer-chasing and improves cache
locality at the common case of 5–20 active price levels per side; see `bench/BASELINE.md`
Phase 4 for the measured effect of the pool-allocator fix on `orders_` lookup.

## Benchmark methodology

Per-op timing on this hardware is quantized to the platform's measured ~41.667ns timer tick
(`mach_timebase_info` numer=125 denom=3, confirmed empirically — there is no userspace cycle
counter available on Apple Silicon). The harness therefore times 128-op groups with 500 warmup
groups discarded per run, rather than timing single operations directly. Reported percentiles
are consequently percentiles of *per-op-equivalent group means*, not single-operation
percentiles — a real tail event in one op is damped, not eliminated, by averaging over its
128-op window. See [`bench/BASELINE.md`](bench/BASELINE.md) for the full derivation, rationale,
and acceptance test.

**Tail percentiles (p99, p999) are not reliably measurable on this setup** and are omitted
above: across 8 repetitions per benchmark, p99 has a coefficient of variation of 27–97% on 6
of 8 benchmarks, and p999 18–43% on all 8 — on an unpinned, multi-tenant machine the measured
tail reflects OS scheduling (preemption, page faults, frequency scaling), not engine behavior.
Only mean and p50 are stable enough to cite single-sample (cv ≤2.81% across all 8 benchmarks).
Stable tail measurement would require core pinning on an otherwise-quiet host, ideally Linux
with isolated CPUs. Full cv table: [`bench/BASELINE.md`](bench/BASELINE.md).

## Research Results

The OFI study below is the **corrected panel study**. It supersedes the original
Iteration 1 / 2 / 3, Research Question 4 and Addition 2 sections, which have been removed
rather than annotated — see "What was removed and why" at the end of this section.

Data: 5 tickers (AAPL, AMZN, ETSY, NFLX, WDAY) × 7 dates, regular session only,
quote-validity-filtered, on a book reconstruction verified free of the `uint32_t` underflow
bug. Full detail: [`results/OFI_STUDY.md`](results/OFI_STUDY.md).

---

### Contemporaneous OFI → return (construction validation)

Does the reconstructed OFI move with contemporaneous mid-price returns, as theory requires?
This is a **validation that the book reconstruction is correct**, not a predictive result.

| Ticker | R² (7-date panel, pooled) |
|---|---|
| AAPL | 0.5579 |
| AMZN | 0.3361 |
| NFLX | 0.3003 |
| WDAY | 0.2436 |
| ETSY | 0.1899 |

The liquidity gradient runs in the direction theory predicts (AAPL highest, ETSY lowest) and is
consistent with the single-date table, though the NFLX/WDAY mid-ranking is noisy at single-date
resolution. **Do not read these as forecasting numbers** — contemporaneous R² measures whether
order flow and price move together within the same interval, which they must.

### Predictive OFI → 1-second forward return

The honest predictive test: train on the 4 earliest dates, test on the 3 latest (~2 months
apart), HAC(5) standard errors. This is a **cross-regime** split, deliberately not walk-forward
— the panel has no daily contiguity.

| Ticker | pooled R²_out |
|---|---|
| AMZN | +0.0062 |
| NFLX | +0.0037 |
| ETSY | +0.0017 |
| AAPL | +0.0014 |
| WDAY | −0.0018 |

Across all 15 ticker × test-date cells, R²_out ranges from **−0.0077 to +0.0069**.

**Every coefficient is HAC(5)-significant (p < 0.05, most p < 0.0001) — and that is a statement
about sample size (N = 35k–88k), not about economic content.** The effect is negligible for all
five tickers. There is no tradeable OFI signal here at a 1-second horizon, and the statistical
significance should not be mistaken for one.

### What was removed and why

The original research section reported: Iteration 1 (raw OFI baseline, AAPL R²_out −0.18% /
AMZN +0.07%), Iteration 2 (normalized OFI), Iteration 3 (10-second horizon decay),
Research Question 4 (cross-asset SPY → AAPL lead-lag), and Addition 2 (multi-frequency decay
across 1s/5s/10s/30s/60s).

All of it was computed on data with three independent, disqualifying defects:

1. **Corrupted book reconstruction** — a `uint32_t` underflow bug meant `ofi` and `mid_price`
   were both derived from a broken book. The original contemporaneous R² landing in a plausible
   literature range (≈0.35–0.45) was coincidence, not confirmation.
2. **Within-day 70/30 split** — training on the first 70% of a single trading day and testing on
   the last 30% crosses the intraday U-shape regime boundary, so train and test are drawn from
   structurally different liquidity regimes.
3. **58% undisclosed extended-hours data** in the underlying sample.

Iterations 1 and 3 and Addition 2 are superseded by the predictive panel table above.
**Iteration 2 (normalized OFI) and Research Question 4 (cross-asset SPY → AAPL) have no
corrected equivalent — they were not re-run on the clean panel, so no result is claimed for
either.** The normalization-hurts conclusion and the no-lead-lag conclusion are both currently
unsupported; they may well be true, but nothing in this repository demonstrates them.

`results/signal_decay.png` was generated from the stale data and has been left in place only as
a build artifact; it is not a result.

---

### Research Question 2 — Spread vs. Volatility Regimes (Glosten-Milgrom)

The Glosten-Milgrom (1985) model predicts that market makers widen spreads in high-volatility regimes to compensate for increased adverse selection risk. To test this, observations are partitioned into quintiles by 5-minute rolling realized variance, and the average quoted spread is measured per quintile.

Spearman rank correlations between realized variance and quoted spread:

| Ticker | ρ (Spearman) | p-value |
|---|---|---|
| AMZN | −0.457 | < 10⁻³⁰⁰ |
| AAPL | −0.386 | < 10⁻³⁰⁰ |

**Note on sign:** the negative correlation appears to contradict Glosten-Milgrom, but is largely an artifact of how quoted spread behaves during the open and close auctions (where the book is thin and spreads are mechanically near zero) coinciding with high realized variance from overnight gaps. During continuous trading hours, within-quintile median spreads are more stable. A cleaner test would restrict to the 9:45–15:45 core trading window and use intraday realized variance. This is a known limitation of the current implementation and a planned improvement.

> **⚠️ Same data vintage as the removed OFI sections.** `docs/FINAL_NUMBERS.md` does not list
> this result as retired, so it is left in place — but both inputs (quoted spread and realized
> variance) come from the same book reconstruction and the same undisclosed-extended-hours
> sample as the OFI work removed above, and this test was **not re-run on the clean 7-date
> panel**. Treat the ρ values as unverified pending a re-run. The stated open/close-auction
> confound is a second, independent reason not to cite the sign.

---

### Research Question 3 — Depth-of-Book Informativeness (Planned)

The feature CSV exports OFI computed at three book depths: L1 (best bid/ask only), L5 (top 5 levels), and L10 (top 10 levels). The hypothesis from Cont et al. is that deeper book information adds marginal predictive power, but with diminishing returns.

The `analysis.py --depth-dir` mode is implemented and ready; running it requires three separate engine passes at different `--depth` settings. Results are pending on a full-day ITCH file. Expected outputs:

```
Depth  1 — Out-of-sample R²: ~0.07%   (baseline)
Depth  5 — Out-of-sample R²: ???
Depth 10 — Out-of-sample R²: ???
```

The direction is ambiguous: deeper book information can add signal (more complete imbalance picture) or add noise (distant levels have weaker price impact and respond to different dynamics).

## Project Structure

```
lob-engine/
├── include/
│   ├── feed/
│   │   ├── itch_parser.h        # Parser interface and callback types
│   │   └── itch_types.h         # ITCH 5.0 message structs (AddOrder, Delete, etc.)
│   ├── book/
│   │   └── order_book.h         # OrderBook class — flat sorted-array price levels
│   └── features/
│       └── feature_engine.h     # FeatureEngine — OFI, spread, trade imbalance
├── src/
│   ├── feed/
│   │   └── itch_parser.cpp      # Binary ITCH 5.0 message dispatch loop
│   ├── book/
│   │   └── order_book.cpp       # Add / delete / replace / execute order logic
│   ├── features/
│   │   └── feature_engine.cpp   # Per-second feature aggregation and CSV writer
│   └── main.cpp                 # Entry point — wires parser → book → features
├── bench/
│   ├── bench_book.cpp           # Microbenchmark: order book operations
│   └── bench_pipeline.cpp       # End-to-end pipeline latency benchmark
├── test/
│   └── test_order_book.cpp      # Unit tests for order book correctness
├── scripts/
│   ├── analysis.py              # OLS regression and R² reporting
│   └── generate_test_data.py    # Synthetic ITCH message generator for testing
├── docs/                        # Design specs and implementation plans
├── Makefile
└── CMakeLists.txt
```

## Data

ITCH 5.0 binary files are available from the [Nasdaq Historical Data](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) portal. Files are on the order of 5–12 GB per trading day and are excluded from this repository via `.gitignore`.

Place downloaded files in `data/` before running.

## References

- Cont, R., Kukanov, A., & Stoikov, S. (2014). [The Price Impact of Order Book Events](https://doi.org/10.1093/jjfinec/nbt002). *Journal of Financial Econometrics*, 12(1), 47–88.
- Nasdaq (2019). [Nasdaq TotalView-ITCH 5.0 Specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf).
