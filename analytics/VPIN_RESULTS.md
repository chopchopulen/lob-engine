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

**AAPL is the standout, and not in the direction naive intuition would predict.** It's the most
liquid, tightest-spread name in the panel, yet its `'C'`-trade Lee-Ready accuracy (40.3%) is the
*worst* of the five — below a coin flip. The reason is visible in the same table: 42.3% of
AAPL's `'C'` trades land exactly at the midpoint, more than 2x any other ticker (13-33%
elsewhere). At the midpoint the quote rule can't disambiguate and falls back to the tick
rule/inheritance, which itself only agrees with ground truth 22.6% of the time for AAPL's
`'C'` trades. Tight, heavily-traded names generating a disproportionate share of exact-midpoint
price-improved prints is a plausible, real market-microstructure effect (sub-penny/midpoint
crossing execution is common in the most liquid names) — this is a genuine finding about
where Lee-Ready struggles, not a scoring artifact (the same near-tautology check that flagged
the `'E'`-circularity issue was re-run here and found nothing analogous for `'C'`).

**Core takeaway for Task 2, stated as instructed**: on a direct sequenced feed, the quote rule
is redundant with the book for lit trades that fill at the touch (`'E'`), genuinely testable
only on the price-improved `'C'` sliver, and completely untestable on the hidden `'P'` flow
where a classifier is actually needed in the absence of book access. `'C'`-only accuracy
(40-86%, ticker-dependent) is the honest number; it is not a stand-in for how well Lee-Ready
would do on `'P'`, only the closest proxy this dataset can produce.

## 3. BVC vs ground truth — the novel per-bucket classification-divergence result

**This is the primary contribution of this layer.** Standard VPIN literature has no way to
validate BVC against a true trade-level classification — it only ever compares BVC to Lee-Ready,
or to itself under resampling. Order-ID linkage in this codebase makes an actual ground-truth
comparison possible for the first time here.

Per volume bucket (reset within each ticker-date, canonical 1/50-ADV sizing, tail bucket
discarded — never carried across a session boundary), buy fraction computed 3 ways: ground
truth (over labelable volume only — `'P'` excluded from both numerator and denominator, since
GT cannot see it), Lee-Ready (over all bucket volume, including `'P'`), and BVC (over all bucket
volume, standardized price change through the normal CDF).

| Ticker | Buckets | BVC MAE (buy-fraction) | Lee-Ready MAE | Avg. GT-labelable fraction of bucket |
|---|---|---|---|---|
| AAPL | 343 | 0.207 | 0.017 | 0.842 |
| AMZN | 343 | 0.180 | 0.036 | 0.668 |
| ETSY | 340 | 0.201 | 0.030 | 0.849 |
| NFLX | 343 | 0.191 | 0.030 | 0.742 |
| WDAY | 343 | 0.211 | 0.043 | 0.748 |
| **Pooled (1,712 buckets)** | | **0.198** | **0.031** | |

**BVC's per-bucket buy-fraction error (MAE ≈ 0.20, on a [0,1]-bounded quantity) is roughly 6x
larger than Lee-Ready's (MAE ≈ 0.03), pooled across the full panel.** This is the headline
number: BVC, which never sees trade-level information at all, is a materially worse estimator
of the true within-bucket buy/sell split than Lee-Ready, which at least sees trade prices and
quotes. Some of Lee-Ready's apparent accuracy here is inherited from bucket composition, not
classifier skill — most bucket volume is `'E'` trades, which Lee-Ready classifies correctly by
the same circularity noted in Section 2, so a bucket's aggregate LR buy-fraction tracks ground
truth well partly *because* most of its volume was never a real test to begin with. This does
not erase the BVC-vs-LR gap — BVC has access to the same bucket composition and still errs far
more — but it does mean Lee-Ready's bucket-level number should not be read as "Lee-Ready solves
classification," only as "Lee-Ready inherits enough tautologically-correct volume per bucket to
outperform a method with even less information."

**Divergence conditions**: Lee-Ready's error correlates with a bucket's hidden-volume fraction
across every ticker (r = 0.25 to 0.57) — it errs more as more of the bucket's volume is
inherently unlabelable, a sensible and honest relationship. **BVC shows no comparable pattern**
(r = -0.05 to +0.12, near-zero and inconsistent in sign) — it is not that BVC degrades
specifically where hidden volume is high, it is uniformly poor regardless of bucket
composition, which is itself informative about what's driving its error (likely dominated by
the standardization/CDF-mapping step rather than the labeling problem hidden volume represents).

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
   book reconstruction with order-ID linkage); the BVC-vs-ground-truth divergence result answers
   a question the standard literature structurally cannot ask, since it has no access to a true
   trade-level classification to compare against.
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
`analytics/panel_task2_lee_ready_C_only.csv`, `analytics/panel_task3_divergence.csv`,
`analytics/panel_task3_divergence_conditions.csv`, `analytics/panel_task3_vpin_counts.json`.
