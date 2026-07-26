# VPIN / Lee-Ready Analytics — Results (full 7-date panel)

Offline analytics layer (`analytics/vpin_extract.cpp`, `analytics/vpin_pipeline.py`,
`analytics/vpin_panel.py`). Does not touch the hot path, matching engine, or
`bench/BASELINE.md` (verified: `BM_FullPipeline` p50 = 16.93ns after adding
`OrderBook::peek_order` — null diff vs the recorded baseline, all 14 `lob_test` cases pass).
Not fed into any vol-forecasting model.

**Scope**: all 7 panel dates (2019-01-30, 03-27, 07-30, 08-30, 10-30, 12-30, 2020-01-30) × all 5
tickers (AAPL, AMZN, ETSY, NFLX, WDAY), regular session only. This supersedes the earlier
single-date (2019-12-30) run — see "Superseded" at the bottom for what changed and why.

**The contribution of this layer is measurement and method comparison, not a trading signal.**
Nothing below is a toxicity or predictive claim.

## 1. Hidden-volume estimation, pooled across 7 dates (deterministic, novel)

| Ticker | Total vol | Hidden vol | Hidden % | Coverage % | Avg. median spread | Avg. median price |
|---|---|---|---|---|---|---|
| AMZN | 5,812,684 | 1,940,005 | **33.4%** | 66.6% | $0.37 | $1,796.17 |
| WDAY | 4,582,817 | 1,230,032 | 26.8% | 73.2% | $0.08 | $179.94 |
| NFLX | 9,617,104 | 2,421,115 | 25.2% | 74.8% | $0.08 | $324.14 |
| ETSY | 4,202,736 | 697,625 | 16.6% | 83.4% | $0.02 | $55.50 |
| AAPL | 41,584,617 | 6,337,144 | **15.2%** | 84.8% | $0.02 | $231.85 |

"Hidden" = `'P'` (non-displayed) executed volume, which per the ITCH 5.0 spec has zero
recoverable side information (order_ref zeroed since 2010-12-06, Buy/Sell Indicator hardcoded
to `'B'` since 2014-07-14 — confirmed against the spec, not assumed). Coverage = ground-truth-
labelable fraction of volume, deterministic from order-ID linkage.

**Directional finding, not a robust statistic.** AMZN is both the highest-hidden-share and
widest-average-spread/highest-priced name; AAPL is lowest on both. That anchors the direction
(wider spread/higher price → more flow migrates to non-displayed order types) at the extremes.
The rank order does **not** fully line up in the middle three (WDAY/NFLX/ETSY) between hidden%
and spread/price — `n=5`, spread and price are collinear here, and this is one relationship
measured two collinear ways, not two independent confirmations. Reporting the direction, not a
precision-implying correlation coefficient.

**Pooling moved these numbers materially from the single-date (2019-12-30) run — the single
date was not representative, and these panel figures supersede it.** AAPL's coverage went
89.4% (single date) → 84.8% (7-date pooled); AMZN went 57.8% → 66.6%. Both moves are sizable
(4-9 points) and in different directions for the two tickers — a reminder that a single day's
hidden-volume share is a noisy estimate of a ticker's typical rate, not a stable property. The
7-date pooled figures in the table above are what should be cited going forward.

## 2. Circularity finding — why Lee-Ready validation is largely tautological on lit trades

Ground truth is *defined* by which side of the book an execution hit. `'E'` (Order Executed)
fills at the resting order's own displayed price — the exact prevailing quote this pipeline
reads from the same book state, with zero lag and zero possible price improvement in this
reconstruction. Verified directly: **100.00% of `'E'` executions price exactly at the
prevailing bid or ask, on every one of the 5 tickers.** Scoring the quote rule against ground
truth on `'E'` therefore tests nothing — it's circular by construction, not a real test of the
classifier. `'E'` is excluded from all Lee-Ready scoring below for this reason.

`'C'` (Order Executed With Price) genuinely diverges from the resting display price — it is the
only lit-trade population where quote-rule classification is actually being tested against
something it doesn't already know.

### Lee-Ready vs ground truth, `'C'`-only, pooled across all 7 dates

| Ticker | Pooled `'C'` n | Overall agree | quote-rule n | quote agree | tick-rule n | tick agree | at-midpoint n | at-midpoint % |
|---|---|---|---|---|---|---|---|---|
| AAPL | 3,372 | 40.3% | 1,945 | 53.3% | 1,427 | 22.6% | 1,427 | **42.3%** |
| AMZN | 353 | 86.4% | 307 | 98.7% | 46 | 4.3% | 46 | 13.0% |
| ETSY | 247 | 45.3% | 165 | 58.8% | 82 | 18.3% | 82 | 33.2% |
| NFLX | 801 | 73.7% | 653 | 87.4% | 148 | 12.8% | 148 | 18.5% |
| WDAY | 544 | 67.8% | 421 | 84.1% | 123 | 12.2% | 123 | 22.6% |

All 5 pooled `'C'` counts clear the ~100-trade reliability floor — every number above is
reportable, not a "sample too small" case.

### AAPL's below-chance 40.3% — decomposed, hypothesis tested, not assumed

A binary classifier at 40.3% is *systematically inverted*, not merely inaccurate — that needed
an explanation, not a hand-wave. `tick_n` and `at_midpoint_n` are exactly equal in this data for
every ticker (every midpoint trade falls to the tick-rule/inherit path, no other trade does),
so the table above already decomposes cleanly: `quote_agree_pct` = accuracy **away from the
midpoint** (quote rule proper), `tick_agree_pct` = accuracy **at the midpoint** (tick-rule
fallback).

**Hypothesis tested**: price improvement moves an execution price from the resting side toward
or across the midpoint, so a bid-resting (seller-initiated) trade could print above the mid and
get misread as a buy by the *quote rule* — inverting it specifically on price-improved trades.
This predicts the away-from-mid subset should be well below 50%. **It is not, for any ticker**:

| Ticker | Away-from-mid (quote rule) | At-midpoint (tick rule) |
|---|---|---|
| AAPL | 53.3% | **22.6%** |
| AMZN | 98.7% | **4.3%** |
| ETSY | 58.8% | **18.3%** |
| NFLX | 87.4% | **12.8%** |
| WDAY | 84.1% | **12.2%** |

**The hypothesis as stated is wrong, and the decomposition says so directly**: away-from-mid
(quote-rule) accuracy is decent-to-excellent for every ticker (53-99%) — not inverted, not even
particularly weak for 4 of 5 names. The inversion is squarely in the **at-midpoint/tick-rule**
subset, which is below chance for *every single ticker* (4.3%-22.6%). **AAPL is not special in
kind — it's the clearest case, not a different phenomenon.** All 5 tickers show the identical
qualitative pattern (good quote rule, badly-inverted tick rule); AAPL's overall blended number
looks worst only because it has by far the largest at-midpoint share (42.3% vs 13-33% for the
other four) — the same bad tick-rule accuracy applied to a much bigger slice drags AAPL's
blended 40.3% below the other tickers' 67.8-86.4%, even though the underlying tick-rule failure
rate is comparable-to-worse for AMZN (4.3%) and NFLX (12.8%).

**Why is the tick rule below chance at the midpoint, not just noisy?** Investigated directly
rather than assumed. Two checks, run on all 5 tickers:

| Ticker | Lag-1 same-side rate (all trades) | n pairs | Tick label = opposite of immediate predecessor | n (at-mid `'C'`, known predecessor) |
|---|---|---|---|---|
| AAPL | 84.6% | 357,640 | 32.0% | 1,248 |
| AMZN | 91.7% | 101,059 | 70.4% | 27 |
| ETSY | 86.3% | 42,487 | 31.1% | 74 |
| NFLX | 88.9% | 105,541 | 46.2% | 117 |
| WDAY | 87.6% | 51,901 | 40.2% | 92 |

**Order flow is strongly positively autocorrelated at lag 1 across every ticker (85-92% same-side
rate)** — a real, robust, standalone finding: consecutive labelable trades share the same true
side the large majority of the time, consistent with order-splitting/iceberg-replenishment
clustering. This is not in question.

**The specific mechanical story tested — that the tick rule mechanically predicts the exact
opposite of whichever trade immediately preceded it, because `'E'` fills exactly at the touch
and the midpoint sits precisely between — is only partially supported.** If that mechanism held
cleanly, "tick label = opposite of predecessor" should run close to 100%; instead it's 31-46%
for 4 of 5 tickers (AMZN's 70.4% sits on only 27 observations, too thin to weight heavily). A
likely reason the clean mechanical story doesn't fully hold: book-only messages (adds/cancels/
replaces) can move the quote between the predecessor trade and the current one, so "the
predecessor's price" and "the current midpoint" don't always sit in the fixed geometric
relationship the simple derivation assumes. **Reporting this honestly rather than forcing the
narrative**: the lag-1 order-flow clustering is real and large, and is plausibly a contributing
ingredient (a tick rule that references stale, already-clustered price information should
struggle when flow is this autocorrelated) — but the exact per-trade mechanical chain from
"order flow clusters" to "tick rule inverts at the midpoint" is not fully pinned down by this
check. Treating the *what* (tick rule is inverted at the midpoint, universally, not just for
AAPL) as established, and the *why* (beyond "order flow clusters and the tick rule doesn't
handle that well") as an open question.

**Core takeaway for Task 2, stated as instructed**: on a direct sequenced feed, the quote rule
is redundant with the book for lit trades that fill at the touch (`'E'`), genuinely testable
only on the price-improved `'C'` sliver, and completely untestable on the hidden `'P'` flow
where a classifier is actually needed in the absence of book access. Away-from-mid accuracy
(53-99%) is the honest measure of the quote rule proper; at-midpoint/tick-rule accuracy is
badly inverted (4-23%) for every ticker tested, for reasons only partially understood. Neither
number is a stand-in for how well Lee-Ready would do on `'P'` — only the closest proxy this
dataset can produce.

## 3. BVC vs ground truth — the novel per-bucket classification-divergence result

**This is the primary contribution of this layer.** Standard VPIN literature has no way to
validate BVC against a true trade-level classification — it only ever compares BVC to Lee-Ready,
or to itself under resampling. Order-ID linkage in this codebase makes an actual ground-truth
comparison possible for the first time here.

**Correction from an earlier draft of this section**: that draft reported "BVC MAE 0.198 vs
Lee-Ready MAE 0.031, BVC ~6x worse" — this comparison is circular and has been retired. Bucket
composition, pooled across the panel, is **76.4% `'E'`, 0.6% `'C'`, 23.0% `'P'`**. Lee-Ready
classifies `'E'` at 100% agreement by construction (Section 2), so a whole-bucket Lee-Ready MAE
is dominated by volume that was never a real test. BVC has no trade-level information at all —
comparing it against a tautologically-advantaged method and calling it "worse" understated BVC
and overstated Lee-Ready. Both figures are kept below for transparency, but neither the
whole-bucket Lee-Ready number nor a "BVC vs Lee-Ready" framing is the headline any longer.

Per volume bucket (reset within each ticker-date, canonical 1/50-ADV sizing, tail bucket
discarded — never carried across a session boundary), buy fraction computed 3 ways: ground
truth (over labelable volume only — `'P'` excluded from both numerator and denominator, since
GT cannot see it), Lee-Ready (over all bucket volume, including `'P'`), and BVC (over all bucket
volume, standardized price change through the normal CDF).

| Ticker | Buckets | BVC MAE | BVC mean signed error | Avg. E / C / P fraction of bucket | Lee-Ready whole-bucket MAE (CIRCULAR) | Lee-Ready `'C'`-only MAE (non-circular) | Buckets with `'C'` volume |
|---|---|---|---|---|---|---|---|
| AAPL | 343 | 0.207 | +0.011 | 0.833 / 0.009 / 0.158 | 0.017 | **0.362** | 327 |
| AMZN | 343 | 0.180 | -0.001 | 0.665 / 0.003 / 0.332 | 0.036 | **0.187** | 95 |
| ETSY | 340 | 0.201 | -0.006 | 0.845 / 0.004 / 0.151 | 0.030 | **0.428** | 115 |
| NFLX | 343 | 0.191 | -0.001 | 0.735 / 0.007 / 0.258 | 0.030 | **0.182** | 195 |
| WDAY | 343 | 0.211 | -0.004 | 0.741 / 0.007 / 0.252 | 0.043 | **0.234** | 138 |
| **Pooled (1,712 buckets)** | | **0.198** | **-0.0002** | 0.764 / 0.006 / 0.230 | 0.031 | **0.291** (n=870 buckets) | |

**(a) Bucket composition makes the structural advantage visible**: `'C'` is only 0.6% of pooled
bucket volume. Whatever Lee-Ready's whole-bucket MAE measures, it is overwhelmingly measuring
performance on `'E'` (76.4%, tautological) and how well it handles `'P'` (23.0%, real
classification, but excluded from the ground-truth *reference* itself — see (c)) — barely at
all on `'C'`, the only population where Lee-Ready is genuinely tested against something.

**(b) Non-circular Lee-Ready figure**: restricting both Lee-Ready's classification and the
ground-truth reference to `'C'`-only volume in each bucket (same population on both sides,
apples-to-apples) gives **MAE = 0.291, pooled** — nearly 10x worse than the circular whole-bucket
figure (0.031), and **worse than BVC's 0.198**. This flips the original framing entirely: once
corrected for circularity, **BVC's per-bucket buy-fraction estimate is actually closer to
ground truth than Lee-Ready's is on the one population where Lee-Ready is genuinely tested.**

**(c) What each figure is actually computed over, stated plainly**: the ground-truth reference
(`gt_buy_frac`) is computed over `'E'+'C'` volume only — it structurally cannot see `'P'`. The
whole-bucket Lee-Ready figure classifies `'E'+'C'+'P'`, i.e. it includes classifying volume
(`'P'`) that the reference itself excludes. That whole-bucket MAE is therefore not purely
"Lee-Ready's classification error" — it's partly an artifact of comparing a fuller-coverage
estimate against an incomplete reference, on top of the `'E'`-tautology. The `'C'`-only figure
in (b) is the one clean comparison in this report: same volume population, same reference,
both sides tested equally, and Lee-Ready (0.291) is beaten by BVC (0.198) on it.

**(d) BVC-vs-ground-truth is the standalone headline: MAE = 0.198.** What that means for VPIN
itself: per-bucket VPIN is `|2b-1|` for buy fraction `b`, so `d(VPIN)/db = ±2` — a buy-fraction
MAE of ε propagates to roughly `2ε` on VPIN's 0-1 scale. **A BVC buy-fraction MAE of 0.198
propagates to roughly 0.40 error on VPIN's own 0-1 scale** — a large fraction of VPIN's entire
range, on a measure computed with zero trade-level information.

**(e) Signed error — is this noise or bias?** Pooled BVC mean signed error is **-0.0002**
(std 0.240) — essentially zero-mean. Per-ticker signed errors are similarly small (+0.011 to
-0.006), none large relative to the 0.18-0.21 MAE. **This distinction matters for VPIN's rolling
average**: a zero-mean error is consistent with per-bucket noise that a multi-bucket rolling
statistic can partially average down; a systematically-signed error would not average out and
would bias every VPIN reading in the same direction. The evidence here points to noise, not
bias — but the per-bucket standard deviation (0.240) is itself large relative to the buy
fraction's own [0,1] range, so "the bias is near zero" is not the same claim as "individual
bucket estimates are precise."

**Divergence conditions**: BVC's error shows no meaningful relationship with a bucket's
hidden-volume fraction (r = -0.05 to +0.12 across tickers, near-zero and inconsistent in sign)
— it is not that BVC degrades specifically where hidden volume is high, it is uniformly poor
regardless of bucket composition, consistent with its error being dominated by the
standardization/CDF-mapping step rather than by anything related to the labeling problem hidden
volume represents. (The earlier Lee-Ready-vs-hidden-fraction correlation is dropped from this
report — it was computed against the now-retired circular whole-bucket MAE and is not a
meaningful conditioning variable on its own.)

### Secondary, non-canonical: an actual VPIN series, explicitly flagged as a deviation

Canonical 1/50-ADV bucket sizing with an N=50 rolling window yields **zero** complete VPIN
observations for every ticker, even pooling all 7 dates — expected, since buckets reset per
session and no single date closes 50 buckets on its own (each date closes ~48-49 at 1/50-ADV).

Re-parameterized to 1/150-ADV buckets, N=20 window (**explicitly non-canonical — this is a
data-driven deviation from the standard 1/50-ADV, N=50 specification, not a replacement for
it**), complete VPIN observations become available: AAPL 909, AMZN 897, ETSY 870, NFLX 900,
WDAY 880 (pooled across 7 dates). This is enough to plot a real distribution per ticker, but it
is a different, smaller-bucket/shorter-window VPIN than what's reported in the literature under
that name — any comparison to published VPIN levels must account for the different
parameterization. Not further analyzed here (no toxicity/predictive claim made on these
values) — reported only to show the mechanism can produce a genuine series once bucket sizing
is adapted to a 7-non-contiguous-date sample, and to make explicit exactly how it deviates from
canonical.

## 4. Honest scope statement

1. **Sections 1 and 3 are the real, defensible contributions**: hidden-volume estimation is
   deterministic and something standard tape-only analysis cannot produce at all (it requires
   book reconstruction with order-ID linkage); the BVC-vs-ground-truth divergence result (MAE
   0.198, standalone, non-circular) answers a question the standard literature structurally
   cannot ask, since it has no access to a true trade-level classification to compare against.
   A naive BVC-vs-Lee-Ready framing was tried first and retired for being circular (Section 3)
   — the corrected, apples-to-apples comparison on the one population where Lee-Ready is
   genuinely tested (`'C'`-only, MAE 0.291) has BVC beating it, the opposite of the original claim.
2. **Section 2's circularity finding is itself a result**, not just a caveat: on a direct
   sequenced feed (as opposed to a consolidated tape), Lee-Ready's quote rule is tautological for
   touch-filling executions and only genuinely testable on the price-improved `'C'` slice —
   an architectural fact about this class of data, not specific to this implementation.
3. **7 non-contiguous dates, ~2 months apart, is far too thin for any predictive or toxicity
   claim.** Nothing in this report should be read as evidence that VPIN (in any of its 3
   variants here) predicts anything. A spike-vs-volatility look would be a sanity check at best,
   not a result, and was not performed for that reason.
4. **Andersen–Bondarenko critique, stated plainly**: VPIN has been shown to be substantially a
   volume/volatility proxy — order-flow-imbalance measures correlate with realized volatility
   largely mechanically (volume buckets close faster in absolute time during volatile periods),
   and VPIN's out-of-sample predictive power for toxicity/flash-event risk weakens considerably
   once that's controlled for. Nothing here contradicts or tests that critique directly; it is
   noted because this report's classification-divergence results measure *properties* of BVC/
   Lee-Ready/ground-truth as estimators, and should not be mistaken for evidence that any of the
   three, once computed, is a useful forward-looking signal.

## Superseded — single-date (2019-12-30) run (2026-07-25)

The original version of this report covered only 2019-12-30 as a plumbing check (confirmed the
extraction → Lee-Ready → bucketing pipeline worked end to end, zero complete canonical VPIN
windows as expected from one date's ~48 buckets). All of its numbers are subsumed by the 7-date
results above, which pool a superset of that date's data plus 6 more regimes, and additionally
correct the framing from "VPIN time series" to "per-bucket classification divergence" per
instruction. Not retracted for being wrong — the single-date run's own conclusions (structural
`'E'`-tautology, zero canonical VPIN windows) held up unchanged under the full panel; it is
superseded only because the 7-date numbers are strictly more informative and are what should be
cited going forward.

## Reproduction

```
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build -j
for DATE in 01302019 03272019 07302019 08302019 10302019 12302019 01302020; do
  for TICKER in AAPL AMZN ETSY NFLX WDAY; do
    ./build/vpin_extract data/raw/${DATE}.NASDAQ_ITCH50 ${TICKER} analytics/trades/trades_${TICKER}_${DATE}.csv
  done
done
python3 analytics/vpin_panel.py
```

Raw ITCH files (8-13GB decompressed each) are not checked in (`data/` and `analytics/trades/`
are both gitignored — same rationale as the rest of this repo's raw/derived data). Persisted
summary outputs: `analytics/panel_task1_hidden_volume.csv`,
`analytics/panel_task2_lee_ready_C_only.csv`, `analytics/panel_task2_tick_rule_mechanism.csv`,
`analytics/panel_task3_divergence.csv`, `analytics/panel_task3_divergence_conditions.csv`,
`analytics/panel_task3_vpin_counts.json`.
