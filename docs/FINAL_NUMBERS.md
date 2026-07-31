# Final Numbers — lob-engine

Single source of truth for every number produced on this project. No narrative — numbers and
their caveats only. If a number isn't here, don't cite it. Machine for all C++ measurements:
Apple M3 Pro (arm64), single core, release build (`-O2`), unpinned/shared host unless noted.

---

## (a) CLAIMABLE

### Hot path / microbenchmarks

| Number | Measured | Caveat |
|---|---|---|
| `BM_FullPipeline` mean: 17.17 ns | `bench/BASELINE.md`, 5 reps, `--benchmark_min_time=2.0s` | Grouped-batch methodology (GROUP_SIZE=128, 500 warmup groups/rep) — required on this host because the Apple Silicon hardware timer quantizes at ~41.667ns, far coarser than single-op latency |
| `BM_FullPipeline` p50: 16.93 ns | Same | Same run. p50 cv ≤2.81% across all 8 benchmarks — stable, safe to cite single-sample |
| Per-benchmark mean/p50, all 8 cases (`BM_AddOrder` 15.19/14.98ns, `BM_DeleteOrder` 15.81/15.63ns, `BM_ReplaceOrder` 30.20/30.27ns, `BM_ExecuteOrder` 14.20/14.00ns, `BM_AddOrder_20L` 8.55/8.46ns, `BM_DeleteOrder_20L` 8.46/8.14ns, `BM_FullPipeline_20L` 10.99/10.74ns) | `bench/BASELINE.md` "Current numbers" table | Mean cv ≤3.05% (loosest: DeleteOrder); p50 cv ≤2.81% — both single-sample-citable. **p99/p999 are NOT** — see Retired/Open |
| Pool-allocator fix: mean reduction -28.6% (AddOrder), -39.7% (DeleteOrder), -34.5% (ReplaceOrder), +0.6%/noise (ExecuteOrder, expected null), -41.8% (AddOrder_20L), -54.0% (DeleteOrder_20L), -27.5% (FullPipeline), -38.7% (FullPipeline_20L) | `bench/BASELINE.md` Phase 4, apples-to-apples pre/post-fix rebuild against the same (corrected) harness | ExecuteOrder's ~0% is a **predicted null**, not a partial fix — that benchmark never touches `orders_` insert/erase. Magnitude is measured against the corrected grouped-batch harness, not the original tick-quantized one |

### End-to-end parser/pipeline throughput

| Number | Measured | Caveat |
|---|---|---|
| Whole-file parse throughput: 18.4–18.8M msg/s (263.24M messages counted by `BM_ParseFile`'s own callback-dispatched definition) | Real main-feed file, 2019-12-30, `LOB_BENCH_ITCH_FILE=... ./lob_bench --benchmark_filter='BM_ParseFile'` | Parse-only — no `OrderBook`/`FeatureEngine` reconstruction. Message count differs from the raw 268,744,780 total frame count (below) because it only includes types with a wired-up callback in that benchmark |
| Real end-to-end throughput (post locate-scan early-exit): ~19.5M msg/s single-pass-equivalent; 14.68s average full-run wall time across 5 study tickers (AAPL/AMZN/ETSY/NFLX/WDAY) | Same file, `./lob_engine <file> <TICKER> <out.csv>`, single merged timer covering locate resolution + parse + reconstruct + write | Full pipeline (parse + book + features), single-ticker filtered. 268,744,780 = exact total message-frame count in this file, verified independently in Python |
| Locate-scan early-exit speedup: 27.63s → 14.68s average, **1.88x** (not 2x) | Same 5 tickers, before/after, single merged timer | Less than the naive 2x a byte-count argument suggests — attributed to page-cache warmth on the early-exit region, measured not assumed. Hot path confirmed unaffected (`BM_FullPipeline` p50 unchanged outside normal run-to-run noise) |

### OFI (order flow imbalance)

| Number | Measured | Caveat |
|---|---|---|
| Contemporaneous OFI→return R², full 7-date panel: AAPL 0.5579, AMZN 0.3361, NFLX 0.3003, WDAY 0.2436, ETSY 0.1899 | `results/OFI_STUDY.md`, regular session, quote-validity-filtered, 7 dates pooled per ticker | Construction validation only, not predictive. Liquidity gradient (AAPL highest, ETSY lowest) confirmed consistent with theory and with the single-date table, though NFLX/WDAY mid-ranking is noisier at single-date resolution |
| Predictive OFI→1s-fwd-return, cross-regime split (train=4 earliest dates, test=3 latest, ~2mo apart, HAC(5)): R²_out ranges -0.0077 to +0.0069 across all 15 ticker×test-date cells; pooled per-ticker R²_out: AAPL 0.0014, AMZN 0.0062, ETSY 0.0017, NFLX 0.0037, WDAY -0.0018 | `results/OFI_STUDY.md` "OFI predictive study on the clean panel" | Near-zero for every ticker, several negative. Every coefficient HAC(5)-significant (p<0.05, mostly p<0.0001) purely from sample size (N=35k-88k) — **explicitly labeled economically negligible for all 5 tickers**, not a tradeable signal. Cross-regime, explicitly not walk-forward (no daily contiguity) |

### VPIN / Lee-Ready analytics layer

| Number | Measured | Caveat |
|---|---|---|
| Hidden-volume coverage, pooled 7 dates: AMZN 66.6%, WDAY 73.2%, NFLX 74.8%, ETSY 83.4%, AAPL 84.8% (hidden% = 33.4/26.8/25.2/16.6/15.2 respectively) | `analytics/VPIN_RESULTS.md` §1, `analytics/panel_task1_hidden_volume.csv` | "Hidden" = `'P'` (non-displayed) volume, spec-confirmed zero recoverable side info (order_ref zeroed since 2010-12-06, Buy/Sell Indicator hardcoded 'B' since 2014-07-14). Single-date (2019-12-30) figures are superseded and **must not be cited**: AAPL was 89.4%, AMZN was 57.8% single-date vs 84.8%/66.6% pooled — a single day is not representative |
| `'E'`-trade circularity: 100.00% of `'E'` executions price exactly at the prevailing bid/ask, all 5 tickers | `analytics/VPIN_RESULTS.md` §2 | Confirms scoring quote-rule against ground truth on `'E'` is tautological by construction (not a bug) — `'E'` excluded from all Lee-Ready scoring for this reason |
| `'C'`-only Lee-Ready decomposition, pooled 7 dates: AAPL 3,372 `'C'` trades (away-from-mid 53.3% @ n=1,945, at-midpoint 22.6% @ n=1,427); AMZN 353 (98.7% @ n=307, 4.3% @ n=46); ETSY 247 (58.8% @ n=165, 18.3% @ n=82); NFLX 801 (87.4% @ n=653, 12.8% @ n=148); WDAY 544 (84.1% @ n=421, 12.2% @ n=123) | `analytics/VPIN_RESULTS.md` §2, `analytics/panel_task2_lee_ready_C_only.csv` | All 5 pooled `'C'` counts clear the 100-trade reliability floor. Away-from-mid = quote rule proper; at-midpoint = tick-rule fallback. Overall/blended `'C'` accuracy per ticker (40.3%/86.4%/45.3%/73.7%/67.8%) should **not** be cited alone — it conflates two very different regimes (see at-midpoint inversion, Open section) |
| BVC vs ground truth, per-bucket buy-fraction MAE: 0.198 pooled (1,712 buckets); mean signed error -0.0002 (essentially zero-mean, std 0.240) | `analytics/VPIN_RESULTS.md` §3 | **This is the primary, non-circular contribution** — BVC has zero trade-level information, so this is a genuine test standard literature cannot run. Propagates to ~0.40 error on VPIN's own 0-1 scale (`d(VPIN)/db=±2`). Do NOT cite "BVC ~6x worse than Lee-Ready" (0.198 vs whole-bucket 0.031) — that comparison is circular (whole-bucket Lee-Ready MAE is inflated by the `'E'`-tautology, 76.4% of bucket volume). The corrected, apples-to-apples figure is Lee-Ready's `'C'`-only bucket MAE = 0.291, pooled — **worse than BVC on this measure**, the opposite of the retired framing |

---

## (b) RETIRED — never cite these again

| Number | Why retired |
|---|---|
| All pre-harness-fix latency figures (41/42/83/84/125/166/167/208/209 ns) | Tick-quantized: Apple Silicon's ~41.667ns hardware timer measured the clock's own resolution, not the engine, on single-op (non-grouped) timing |
| README p99/p999 latency figures (22.46ns / 45.90ns) | Not wrong, unmeasurable: p99 cv 27-97% on 6/8 benchmarks, p999 cv 18-108% on all 8, dominated by OS scheduling noise on this unpinned host, not engine behavior |
| README `items_per_second` throughput-derived latency table | Derived from the tick-quantized old harness; no substitute offered because re-deriving from new numbers wouldn't be a real measurement |
| 226M messages / 7.8M msg/s / 29.0s end-to-end throughput claim | Traced to source (repo's initial commit): the parser had **zero ticker filtering anywhere** at that point (`// stock_locate (we skip it)`) — "single-ticker mode" fed every message in the file into one merged, nonsensical cross-instrument book. `msg_count` under that code is essentially the whole file's message count. Not correctable to a new number — no architecture this codebase has ever had reconstructs all-instrument books simultaneously |
| Interim "~18.8k msg/s" end-to-end draft figure | Mismatched-denominator artifact: filtered-match-count numerator (e.g. 1.5M for AAPL) divided by a full-file-scan-time denominator that (at the time) also excluded half the real cost. Diagnosed and corrected — see claimable "Real end-to-end throughput" above |
| OFI Iteration 1 within-day 70/30 split (AAPL R²_out=-0.18%, AMZN R²_out=+0.07%) | Chronological split within a single day crosses the intraday U-shape regime boundary; 58% of underlying data was undisclosed extended-hours. Additionally computed on since-found-stale/corrupted data |
| 2026-07-24 OFI re-audit numbers (day-level split, walk-forward, HAC on the old panel) | Methodology was correct and reused as-is for the clean re-run; the numbers themselves were computed on stale/corrupted `data/` CSVs (see Blocker 1 below) |
| Original contemporaneous OFI table (AAPL R²≈0.35-0.45 range on stale data) | `ofi` and `mid_price` were both derived from the same corrupted book reconstruction (uint32_t-underflow bug) — landing in a plausible literature range was coincidence, not confirmation. Corrected on verified-clean data: AAPL 0.62 (single date) / 0.5579 (7-date panel) |
| BVC-vs-Lee-Ready "BVC ~6x worse" framing (0.198 vs whole-bucket 0.031) | Circular: whole-bucket Lee-Ready MAE is inflated by the `'E'`-tautology (76.4% of bucket volume is `'E'`, classified at 100% agreement by construction). See claimable BVC entry for the corrected comparison |
| "42% of AAPL's `'C'` trades land at the midpoint" as a complete explanation for its 40.3% blended accuracy | Not wrong as a description, but insufficient as an explanation — see Open section, tick-rule inversion cause is uncharacterized, not "explained by midpoint share" |
| Research Question 2 — spread vs. volatility regimes (Spearman ρ: AMZN −0.457, AAPL −0.386, both p < 10⁻³⁰⁰) | Same data provenance as the retired OFI iterations: both inputs (quoted spread, realized variance) derive from the book reconstruction corrupted by the `uint32_t`-underflow bug, on the undisclosed-extended-hours sample. Never re-run on the clean 5-ticker × 7-date panel, so **no corrected equivalent exists and no result is claimed** — same disposition as OFI Iteration 2 (normalized OFI) and Research Question 4 (cross-asset SPY→AAPL). The negative sign also carried an acknowledged open/close-auction confound, an independent second reason not to cite it. Table removed from the README |
| `results/signal_decay.png` | Generated from the stale/corrupted dataset behind the retired OFI iterations. Removed from version control as a build artifact of a retired dataset; it is not a result and must not be cited |

---

## (c) OPEN — known, not resolved

| Item | Status |
|---|---|
| ASan/UBSan build hangs indefinitely | Logged, not diagnosed (`audit/FINDINGS.md`). Reproduces on code predating this session's changes (ruled out the pool-allocator fix as cause). Three invocations spun 15-54+ minutes before being killed, no crash, no output. Hypothesis (macOS ASan runtime pathology, shadow-memory init) is unconfirmed — a guess, not a diagnosis. Plain `-O2` Release build has never shown this behavior |
| Tail percentiles (p99/p999) require core pinning to be trustworthy | `bench/BASELINE.md` p50/p99 stability check: p99 cv is 27-97% on 6 of 8 benchmarks on this unpinned, shared host. Only `BM_ReplaceOrder` (1.64%) and `BM_FullPipeline_20L` (2.84%) show stable p99. A trustworthy single-sample tail-percentile measurement would need a pinned core on an otherwise-quiet host — not attempted |
| p99 cv spread across benchmarks (27-97%) is itself unexplained | Same stability check: `BM_DeleteOrder`'s p99 cv of 96.93% vs `BM_ReplaceOrder`'s 1.64% — an order-of-magnitude difference in tail stability between benchmarks with no established root cause; noted, not investigated |
| `order_ref` collision handling (`replace_order`/`add_order`) silently orphans share-count on overwrite | `audit/FINDINGS.md` item 5 — a defensive gap, not reachable on a clean ITCH feed (exchange guarantees ref uniqueness), but no fix and no measured collision rate on real data exist. Confirmed present, not exploited by any data used in this project |
| Tick-rule inversion at the midpoint (4.3%-22.6% accuracy, all 5 tickers, below chance) — cause | **Measured, confirmed real, cause uncharacterized.** Sign bug ruled out directly against spec/code: tick direction (uptick=buy/downtick=sell), "previous trade" reference (genuinely the previous trade, not a quote/message), and ground-truth convention (resting-on-bid=seller) all verified correct, cross-checked via consistent quote-rule accuracy (53-99%, all tickers — an inverted GT convention would also invert this, and it doesn't). Lag-1 trade-side persistence is confirmed strong (84.6-91.7%, all tickers) but the specific mechanism tested for connecting persistence to the inversion (a midpoint trade mechanically registering opposite its touch-adjacent predecessor) was tested and refuted: accuracy is comparably poor regardless of whether the previous trade printed at bid/ask/neither, and `'inherit'`-derived labels (the dominant path, 81.6% of AAPL's at-midpoint trades) show no better accuracy than freshly-computed `'tick'` labels. Do not cite a mechanism for this finding — state the numbers only |

---

## Reproduction pointers

- Microbenchmarks: `bench/BASELINE.md` "How to reproduce"
- End-to-end/throughput: `bench/BASELINE.md` "Real end-to-end measurement" and "Locate-scan early-exit" sections
- OFI: `scripts/ofi_panel_study.py`, `results/OFI_STUDY.md`
- VPIN/Lee-Ready: `analytics/vpin_panel.py`, `analytics/VPIN_RESULTS.md`
- Full test suite: `./build/lob_test` (16 cases, all passing at time of writing)
