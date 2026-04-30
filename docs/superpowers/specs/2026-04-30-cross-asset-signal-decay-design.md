# Design: Cross-Asset OFI & Multi-Frequency Signal Decay

**Date:** 2026-04-30  
**Status:** Approved

---

## Overview

Two research additions to the LOB engine:

1. **Cross-Asset OFI** — test whether SPY OFI leads AAPL returns by 1–2 seconds (ETF-first macro hypothesis)
2. **Multi-Frequency Signal Decay** — measure how OFI predictive power decays as aggregation horizon grows from 1s to 60s

---

## Addition 1 — Cross-Asset OFI (SPY → AAPL)

### Hypothesis

Index ETFs (SPY) reprice faster than single stocks in response to macro news. SPY OFI at time t should therefore predict AAPL returns at time t+1 (lag-1) and possibly t+2 (lag-2), while AAPL's own OFI at t predicts t+1 with lower or no R².

### C++ Changes — `main.cpp` only

**New CLI mode (argc == 5):**
```
./lob_engine <itch_file> <TICKER1> <TICKER2> <combined.csv>
```
Single-ticker mode (`argc == 4`) is unchanged and backward compatible.

**Dual-ticker routing architecture:**

The parser is called with `filter_stock = ""` (no filtering at the parser level). `main.cpp` owns the routing logic:

- `unordered_map<uint64_t, uint8_t> order_route` — maps `order_ref → {0 = ticker1, 1 = ticker2}`
- `on_add`: checks `m.stock` against both tickers; if it matches, inserts into `order_route` and dispatches to the correct `(OrderBook, FeatureEngine)` pair; otherwise skips
- `on_delete` / `on_replace` / `on_execute`: look up `order_route`, skip if absent, route to correct pair, erase from map on delete or full execute

**No changes to:** `itch_parser`, `order_book`, `feature_engine`, `itch_types`

**Post-parse merge:**

After parsing, `main.cpp` merges `feat1.rows()` and `feat2.rows()` using second-floor bucketing:

```
bucket_key = timestamp_ns / 1_000_000_000
```

Build a `map<uint64_t, pair<FeatureRow*, FeatureRow*>>` keyed by bucket. Write only rows where both tickers have data for that second. The `ts` column in the combined CSV is the actual timestamp of the ticker-1 row for that second.

**Combined CSV schema (written by C++):**
```
ts, {t1}_mid, {t1}_ofi, {t2}_mid, {t2}_ofi
```
Column prefixes are derived from the ticker names passed on the command line — not hardcoded to AAPL/SPY.

**Edge cases:**
- If either ticker produces zero rows (ticker not in ITCH file), print a clear warning and exit without writing the CSV
- `order_route` map peak size: ~50k entries at any time; negligible memory impact

### Python Changes — `analysis.py`

**New function:** `cross_asset_ofi(combined_csv)`

1. Load combined CSV, parse `ts` as datetime index
2. Compute forward returns in Python (avoids any lookahead in C++):
   ```python
   df['aapl_fwd_ret_1s'] = df['aapl_mid'].pct_change().shift(-1)
   df['spy_ofi_lag1']    = df['spy_ofi'].shift(1)
   df['spy_ofi_lag2']    = df['spy_ofi'].shift(2)
   ```
3. Run five regressions on the out-of-sample 30% (time-series 70/30 split, same as Q1–Q3):

   | Model | Predictor(s) | Target |
   |---|---|---|
   | 1 | `aapl_ofi` | `aapl_fwd_ret_1s` |
   | 2 | `spy_ofi_lag1` | `aapl_fwd_ret_1s` |
   | 3 | `spy_ofi_lag2` | `aapl_fwd_ret_1s` |
   | 4 | `[aapl_ofi, spy_ofi_lag1]` | `aapl_fwd_ret_1s` |
   | 5 | `[aapl_ofi, spy_ofi_lag1, spy_ofi_lag2]` | `aapl_fwd_ret_1s` |

4. Report coefficient(s), R²\_in, R²\_out for all five models. Print ΔR²\_out for models 4 and 5 relative to model 1.

**Lag-2 interpretation:** if Model 3 R²\_out > 0 and comparable to Model 2, the SPY→AAPL lead persists beyond 1 second, suggesting slower information transmission. If Model 3 R²\_out ≈ 0 while Model 2 > 0, the lead is a 1-bar effect only.

---

## Addition 2 — Multi-Frequency Signal Decay

### Hypothesis

OFI predictive power peaks at short horizons (≤1s) and decays toward zero at longer horizons as information is incorporated. AMZN should show higher R² than AAPL at every horizon due to slower arbitrage on a less liquid instrument.

### Python Changes — `analysis.py`

**New function:** `signal_decay(aapl_csv, amzn_csv)`

No C++ changes. Uses existing single-ticker CSVs.

**For each ticker and each horizon in `[1, 5, 10, 30, 60]` seconds:**
1. Resample the 1s CSV to the target horizon:
   - `ofi`: sum (accumulate all order flow imbalance within the window)
   - `mid_price`: last (end-of-window price)
2. Compute forward return: `mid_price.pct_change().shift(-1)` at that horizon
3. Run OFI → forward return regression (70/30 time-series split)
4. Record R²\_out

**Output:** `results/signal_decay.png`
- X-axis: horizon in seconds (log scale recommended; 1, 5, 10, 30, 60)
- Y-axis: out-of-sample R²
- Two lines: AAPL and AMZN overlaid on the same axes
- Horizontal reference line at R²=0

**New dependency:** `matplotlib` (for PNG output)

---

## CLI Summary

```bash
# Single-ticker (unchanged)
./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv

# Dual-ticker cross-asset (new)
./lob_engine data/01302020.NASDAQ_ITCH50 AAPL SPY data/cross_AAPL_SPY.csv

# Analysis — single ticker only
python scripts/analysis.py data/features_AAPL.csv

# Analysis — with cross-asset Q4
python scripts/analysis.py data/features_AAPL.csv --cross data/cross_AAPL_SPY.csv

# Analysis — with signal decay
python scripts/analysis.py data/features_AAPL.csv --decay data/features_AMZN.csv

# Analysis — full suite
python scripts/analysis.py data/features_AAPL.csv \
    --cross data/cross_AAPL_SPY.csv \
    --decay data/features_AMZN.csv
```

---

## Files Changed

| File | Change |
|---|---|
| `src/main.cpp` | Dual-ticker mode, routing map, combined CSV writer |
| `scripts/analysis.py` | `cross_asset_ofi()`, `signal_decay()`, updated `main()` |

No other files modified.

---

## Definition of Done

1. `make` builds cleanly, no new warnings
2. `./lob_engine <itch> AAPL SPY data/cross_AAPL_SPY.csv` runs in a single pass; exit summary shows both tickers' message counts
3. `data/cross_AAPL_SPY.csv` has ~55k rows, 5 columns, no missing data in first inspection
4. `python scripts/analysis.py ... --cross ... --decay ...` runs to completion, prints Q4 table with 5 models and lag-1/lag-2 R² values
5. `results/signal_decay.png` exists with two labeled lines

---

## References

- Cont, R., Kukanov, A., & Stoikov, S. (2014). The Price Impact of Order Book Events. *Journal of Financial Econometrics*, 12(1), 47–88.
- Glosten, L. R., & Milgrom, P. R. (1985). Bid, ask and transaction prices in a specialist market with heterogeneously informed traders. *Journal of Financial Economics*, 14(1), 71–100.
