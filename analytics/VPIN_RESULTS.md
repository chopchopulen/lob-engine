# VPIN / Lee-Ready Analytics — Results (single date, 2019-12-30)

Offline analytics layer (`analytics/vpin_extract.cpp`, `analytics/vpin_pipeline.py`).
Does not touch the hot path, matching engine, or `bench/BASELINE.md` (verified: `BM_FullPipeline`
p50 = 16.93ns after adding `OrderBook::peek_order` — null diff vs the recorded baseline, all
14 `lob_test` cases still pass). Not fed into the vol-forecasting model.

Scope: **one date, all 5 panel tickers.** This validates the ground-truth → Lee-Ready → VPIN
pipeline mechanically end to end. It is explicitly **not** a predictive or toxicity result —
see "Limits" below.

## Task 1 — trade extraction, ground-truth coverage

| Ticker | Trades (E+C+P) | GT-labeled (E/C) | GT coverage |
|---|---|---|---|
| AAPL | 67,721 | 60,543 | 89.4% |
| AMZN | 26,144 | 15,123 | 57.8% |
| ETSY | 4,591 | 4,106 | 89.4% |
| NFLX | 16,092 | 11,791 | 73.3% |
| WDAY | 4,780 | 3,799 | 79.5% |

Ground truth = order-ID linkage (resting order's side, looked up before `execute_order()`
mutates it). The uncovered fraction is entirely `'P'` (non-displayed/hidden) volume, which per
the ITCH 5.0 spec (section 1.5.1 + revision history) has zero recoverable side information —
Order Reference Number zeroed since 2010-12-06, Buy/Sell Indicator hardcoded to `'B'` since
2014-07-14. This is a spec fact, not a gap in this extractor.

## Task 1b — coverage is a hidden-volume estimate, not just a caveat

| Ticker | Total vol (reg. session) | Hidden vol | Hidden % | Median spread | Median price |
|---|---|---|---|---|---|
| AAPL | 5,678,176 | 651,294 | 11.5% | $0.02 | $290.78 |
| AMZN | 717,078 | 312,932 | 43.6% | $0.43 | $1,851.00 |
| ETSY | 364,333 | 44,575 | 12.2% | $0.01 | $44.67 |
| NFLX | 727,295 | 218,438 | 30.0% | $0.07 | $325.99 |
| WDAY | 285,389 | 73,965 | 25.9% | $0.07 | $163.93 |

Hypothesis: hidden-volume share rises with price/spread. **Holds, on this single date:**
Pearson r(hidden%, median spread) = **0.875**, r(hidden%, median price) = **0.820**. AMZN, the
highest-priced/widest-spread name, has by far the highest hidden share (43.6%); AAPL and ETSY,
the tightest/cheapest, have the lowest (~11-12%). Consistent with a simple mechanism: wider
absolute spreads make displayed limit orders costlier to rest at the touch, pushing more flow
to non-displayed order types. n=5 (one point per ticker, single date) — directionally
suggestive, not a fitted/tested relationship; would need the full 7-date panel to say more.
Reporting this as its own finding per instruction, not merely a VPIN-coverage caveat: **percent
of executed volume that is unlabelable-by-construction is itself a measurable per-ticker
market-structure statistic**, independent of anything downstream.

## Task 2 — Lee-Ready vs ground truth

Contemporaneous quote rule (trade price vs `(bid+ask)/2` at time of trade) + tick-rule fallback
at the exact midpoint, zero-tick inherits the prior label. **No 5-second lag** — that lag in the
original 1991 paper compensates for asynchronous trade/quote reporting on a consolidated tape;
this is a single sequenced feed where book state is available with no lag, so the lag doesn't
apply here.

| Ticker | n (lit) | Overall agree | 'E' agree | 'C' agree | quote-rule n | quote agree | tick n | tick agree | inherit n | inherit agree | at-midpoint |
|---|---|---|---|---|---|---|---|---|---|---|---|
| AAPL | 60,060 | 99.73% | 100.0% (n=59,754) | 46.7% (n=306) | 59,907 | 99.91% | 35 | 28.6% | 118 | 28.8% | 0.25% |
| AMZN | 14,759 | 99.99% | 100.0% (n=14,734) | 92.0% (n=25) | 14,757 | 100.0% | 1 | 0.0% | 1 | 0.0% | 0.01% |
| ETSY | 4,097 | 99.85% | 100.0% (n=4,080) | 64.7% (n=17) | 4,091 | 99.95% | 2 | 50.0% | 4 | 25.0% | 0.15% |
| NFLX | 11,684 | 99.91% | 100.0% (n=11,621) | 84.1% (n=63) | 11,674 | 100.0% | 3 | 0.0% | 7 | 0.0% | 0.09% |
| WDAY | 3,793 | 99.68% | 100.0% (n=3,765) | 57.1% (n=28) | 3,786 | 99.87% | 1 | 0.0% | 6 | 0.0% | 0.18% |

**Critical framing — do not read the overall/quote-rule numbers as "Lee-Ready is 99.9%
accurate."** They are near-tautological, not a genuine classifier result:

- `'E'` (Order Executed) fills at the resting order's own displayed price. That price is *by
  construction* exactly the prevailing best bid or best ask this pipeline just read off the same
  book state, before the fill mutated it. There is no lag and no price improvement possible in
  this reconstruction, so the quote rule mechanically recovers ground truth for every single
  `'E'` trade (verified: 100.00% of `'E'` executions price exactly at the prevailing bid or ask,
  all 5 tickers). This is checked, not assumed — see the sanity check the request asked for:
  a suspiciously round/near-perfect number *is* a bug-shaped result, and this is what it turned
  out to be — a structural artifact of the comparison, not a scoring bug.
- `'C'` (Order Executed With Price) genuinely diverges from the resting display price — this is
  the *only* subset where quote-rule classification is actually being tested against something
  it doesn't already know. `'C'` volume is a small fraction of trades (0.04%-1.3% of executions
  across tickers) but its accuracy is structured and realistic: 46.7% (AAPL) to 92.0% (AMZN),
  landing in/near the range the literature actually reports for real quote-rule classification
  (roughly 70-90% is typical; several of these are lower, likely because `n` is small — 17 to
  306 trades — and 'C' executions specifically occur away from the touch, which is exactly the
  harder case for a quote-rule classifier by construction).
- Tick-rule fallback and midpoint-inherit cases are rare (≤0.25% of lit trades per ticker) and
  their accuracy (0-50%) is close to a coin flip on tiny samples (n=1 to 118) — not a reliable
  estimate either way, just flagged for completeness.
- Spread-quartile accuracy (computed on the full lit set) is ~99-100% in every quartile for
  every ticker — flat across conditions. Per the request's own sanity rule ("if errors are FLAT
  across conditions, suspect the scoring, not the classifier"): correct diagnosis, and the cause
  is the same `'E'`-tautology above, since `'E'` dominates every quartile bucket. A
  quartile breakdown restricted to `'C'`-only trades would be the honest version of this cut, but
  per-ticker `'C'` counts (17-306) are too small to slice into quartiles reliably on one date.

**Bottom line for Task 2**: the real, defensible finding is the `'C'`-only accuracy (46.7%-92.0%,
ticker-dependent, small-n), not the 99.9% headline. And that gap matters directly for what
Lee-Ready is actually *for* here: its job is classifying trades with **no** ground truth
available (i.e. `'P'` hidden trades), which structurally resemble `'C'` far more than `'E'`
(price not fixed to a known resting level) — so the accuracy that would transfer to the regime
Lee-Ready is actually needed for is closer to the `'C'`-only numbers than the 99.9% aggregate.
This gap is largest for AMZN, which also has the largest hidden-volume share (43.6%, Task 1b) —
the ticker where Lee-Ready matters most is also the one with the fewest `'C'` trades (n=25) to
validate against, so confidence in AMZN's hidden-trade classification is the weakest of the five.

## Task 3 — VPIN, three variants (plumbing check only)

Bucket size = that ticker's regular-session executed volume ÷ 50; buckets close at cumulative
volume; trailing partial bucket discarded (never carried past session end — there is only one
session per file). VPIN = rolling mean of `|buy-sell|/volume` over the last 50 closed buckets,
computed 3 ways off the same bucket boundaries: ground truth, Lee-Ready, and BVC (bucket price
change standardized and passed through the normal CDF).

| Ticker | Buckets closed | Bucket size (shares) | Complete 50-bucket VPIN windows |
|---|---|---|---|
| AAPL | 49 | ≈113,564 | 0 |
| AMZN | 49 | ≈14,342 | 0 |
| ETSY | 48 | ≈7,287 | 0 |
| NFLX | 49 | ≈14,546 | 0 |
| WDAY | 49 | ≈5,708 | 0 |

**Zero complete VPIN observations on any ticker, as anticipated in the approved plan**: 1/50th-
ADV bucket sizing needs a full day's volume just to close ~50 buckets, and a rolling 50-bucket
window needs 50 *closed* buckets before it can emit a single VPIN value — one date closes 48-49
buckets, one short. This is not a bug; it's the direct, predicted consequence of pairing
per-session bucket sizing with an N=50 window on single-day data (Part B.3 of the approved
plan). **This run's purpose was to validate the three-way bucketing/VPIN plumbing mechanically
— it does that (buckets close correctly, all 3 buy/sell splits compute without error) — not to
produce a VPIN reading.** A real VPIN comparison needs the 7-date panel, which will supply
enough cumulative closed buckets (or, alternatively, a smaller `N` / smaller bucket size — an
open parameter choice, not decided here) to produce actual observations.

## Task 4 — honest scope statement

1. **The real contribution is the three-way comparison (ground truth vs Lee-Ready vs BVC), not
   the VPIN numbers themselves.** Standard literature has no way to compute a genuine
   ground-truth trade classification — it always validates Lee-Ready/BVC against *each other*
   or against sparse, indirect proxies. This codebase's live book reconstruction can compute
   exact ground truth for lit (`'E'`/`'C'`) trades via order-ID linkage, which is what makes
   Task 2's `'C'`-only accuracy numbers meaningful in a way the aggregate can't be.
2. **Coverage-as-hidden-volume (Task 1b) is a standalone finding**, not just a VPIN caveat: the
   fraction of volume that's unlabelable by construction (`'P'` trades) correlates with spread
   and price across these 5 tickers (r=0.88, r=0.82) on this one date.
3. **A single date is a plumbing check, not a result.** Zero complete VPIN windows on any
   ticker (Task 3) is the direct, expected consequence — not a finding about toxicity, volatility,
   or anything predictive. Any claim beyond "the mechanism works" requires the 7-date panel.
4. **Andersen-Bondarenko critique, stated plainly, not glossed over**: VPIN has been shown to be
   substantially a volume/volatility proxy — order-flow imbalance measures correlate heavily with
   realized volatility by construction (volume buckets are wider in absolute time during volatile
   periods), and VPIN's out-of-sample predictive power for toxicity/flash-event risk weakens
   considerably once that's controlled for. Nothing in this report is a toxicity or predictive
   claim — this measures properties of three classification/aggregation methods against each
   other and against a rare ground-truth baseline, it does not endorse VPIN as a trading or
   risk signal.

## Reproduction

```
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build -j
./build/vpin_extract data/raw/12302019.NASDAQ_ITCH50 <TICKER> analytics/trades/trades_<TICKER>_12302019.csv
python3 analytics/vpin_pipeline.py
```

Raw ITCH file for 2019-12-30 (8.25GB decompressed) re-downloaded for this run via chunked
range requests (`data/raw/` is gitignored — not checked in). Intermediate outputs:
`analytics/task1b_coverage.csv`, `analytics/task2_lee_ready_summary.csv`,
`analytics/task3_vpin_results.json`, `analytics/trades/trades_<TICKER>_12302019.csv`.
