# Normalized OFI — Design Spec

**Date:** 2026-04-29
**Status:** Approved

## Problem

Raw OFI is measured in shares. A 100-share OFI on a thinly-traded stock looks identical to 100 shares on AAPL, which trades millions of shares daily. This makes cross-instrument comparison and regression signal meaningless.

## Solution

Normalize OFI by the average executed trade size over a rolling 60-second window (Cont et al. 2014):

```
normalized_ofi = raw_ofi / avg_trade_size
avg_trade_size = rolling_executed_shares / rolling_trade_count
```

This makes OFI dimensionless and comparable across instruments.

## Approach

**Separate 60-second deque** (Approach A) — mirrors the existing `trade_window_` pattern exactly. The existing 10-second trade imbalance window is untouched. Two deques, cleanly separated concerns, O(n) scan at snapshot time (negligible: once per second on sparse trade data).

## Changes

### `include/features/feature_engine.h`

**`FeatureRow` struct** — add one field after `ofi_l10`:
```cpp
double normalized_ofi;  // ofi / avg_trade_size; NaN when no trades in 60s window
```

**`FeatureEngine` private state** — add alongside existing `trade_window_` members:
```cpp
struct NormTradeEvent { uint64_t ts; uint32_t shares; };
std::deque<NormTradeEvent> norm_window_;
uint64_t norm_window_ns_ = 60ULL * 1'000'000'000ULL;
```

### `src/features/feature_engine.cpp`

**`on_trade`** — push to `norm_window_` and evict stale entries (after the existing `trade_window_` block):
```cpp
norm_window_.push_back({ts, shares});
while (!norm_window_.empty() &&
       (ts - norm_window_.front().ts) > norm_window_ns_)
    norm_window_.pop_front();
```

**`on_book_update`** — at the second-boundary snapshot, after setting `row.ofi`:
```cpp
double norm_shares = 0.0;
uint64_t norm_count = 0;
for (const auto& e : norm_window_) { norm_shares += e.shares; ++norm_count; }
double avg_trade_size = (norm_count > 0) ? (norm_shares / norm_count) : 0.0;
row.normalized_ofi = (avg_trade_size > 0.0)
    ? (row.ofi / avg_trade_size)
    : std::numeric_limits<double>::quiet_NaN();
```

**`write_csv`** — append `normalized_ofi` column to header and each row.

### `scripts/analysis.py`

- Generalize `ofi_return_regression` to accept `ofi_col='ofi'` parameter
- Call it twice in `main()`: once for `'ofi'`, once for `'normalized_ofi'` (guarded by column-existence check)

## Edge Cases

- **No trades in 60s window** → emit `NaN`; pandas `dropna()` excludes these rows from regression automatically.
- **avg_trade_size very small but nonzero** → no special handling; the normalized value will be large but finite, which is the correct behavior.

## Success Criteria

Re-run on ITCH data for AAPL and AMZN and report:
- Raw OFI R² (baseline: AAPL ~0%, AMZN ~0.07%)
- Normalized OFI R² for both tickers
- Whether normalization improves the out-of-sample R²
