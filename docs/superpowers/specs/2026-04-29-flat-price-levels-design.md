# Flat Price Level Array — Design Spec
**Date:** 2026-04-29
**Scope:** Replace `std::map` with a hybrid flat sorted array + overflow map for price levels in `OrderBook`

---

## Goal

Improve per-message latency by replacing the `std::map`-backed price level store with a cache-friendly flat sorted array for the top `TOP_LEVELS = 20` price levels. Levels beyond the top 20 overflow to a `std::map`. The public API of `OrderBook` is unchanged.

**Baseline (std::map, Apple Silicon, -O2):**
- `BM_AddOrder`     p50=41ns  p99=42ns
- `BM_DeleteOrder`  p50=42ns  p99=83ns
- `BM_FullPipeline` p50=42ns  p99=84ns

---

## Assumptions

- N = `TOP_LEVELS = 20` is a compile-time constant (`constexpr uint32_t`). The compiler can unroll inner loops over the array.
- `orders_` (unordered_map for O(1) order lookup by ref) is unchanged.
- All public methods (`add_order`, `delete_order`, `replace_order`, `execute_order`, `top_of_book`, `bid_levels`, `ask_levels`) keep their existing signatures.
- Only `order_book.h` and `order_book.cpp` change. All other files (`feature_engine`, `itch_parser`, `main.cpp`, bench files) require no modification.

---

## New Types (order_book.h)

```cpp
static constexpr uint32_t TOP_LEVELS = 20;

struct PriceLevel {
    uint32_t price;
    uint32_t shares;
};

// Flat sorted array of up to TOP_LEVELS price levels.
// Always sorted ascending by price (index 0 = lowest price).
// Best bid  = data[size-1]  (highest price in array)
// Best ask  = data[0]       (lowest price in array)
struct FlatLevels {
    std::array<PriceLevel, TOP_LEVELS> data{};
    uint32_t size = 0;
};
```

## Updated Private Members (OrderBook)

Replace:
```cpp
std::map<uint32_t, uint32_t> bids_;
std::map<uint32_t, uint32_t> asks_;
```

With:
```cpp
FlatLevels                   flat_bids_;      // top TOP_LEVELS bid prices (closest to spread)
FlatLevels                   flat_asks_;      // top TOP_LEVELS ask prices (closest to spread)
std::map<uint32_t, uint32_t> overflow_bids_;  // bid prices outside top TOP_LEVELS
std::map<uint32_t, uint32_t> overflow_asks_;  // ask prices outside top TOP_LEVELS
```

---

## add_to_level Logic

### Bids (want highest prices in flat array)

Flat array sorted ascending → worst bid = `data[0]` (lowest price), best bid = `data[size-1]`.

1. Scan flat array for existing `price` → add `shares` to that entry. Done.
2. Price not found in flat array:
   a. `size < TOP_LEVELS` → insert at sorted position (shift right). Done.
   b. `size == TOP_LEVELS` and `price > data[0].price` (better than worst flat level):
      - Demote `data[0]` to `overflow_bids_[data[0].price] = data[0].shares`
      - Shift array left by one to close the gap
      - Insert `price` at sorted position. Done.
   c. `size == TOP_LEVELS` and `price <= data[0].price` → `overflow_bids_[price] += shares`. Done.

### Asks (want lowest prices in flat array)

Flat array sorted ascending → best ask = `data[0]`, worst ask = `data[size-1]`.

1. Scan flat array for existing `price` → add `shares`. Done.
2. Price not found:
   a. `size < TOP_LEVELS` → insert at sorted position. Done.
   b. `size == TOP_LEVELS` and `price < data[size-1].price` (better than worst flat level):
      - Demote `data[size-1]` to `overflow_asks_[data[size-1].price] = data[size-1].shares`
      - Insert `price` at sorted position. Done.
   c. `size == TOP_LEVELS` and `price >= data[size-1].price` → `overflow_asks_[price] += shares`. Done.

---

## remove_from_level Logic

### Bids

1. Scan flat array for `price`:
   - Found → reduce `shares`. If `shares > 0`, done.
   - Shares hit zero → remove entry (shift array left to close gap), `size--`.
     - If `overflow_bids_` is non-empty: promote `overflow_bids_.rbegin()` (highest overflow bid — it is the next-best level) → append to `data[size]`, `size++`, erase from overflow. Done.
   - Not found in flat array → look up `overflow_bids_[price]`, reduce shares. If zero, erase from map. Done.
   - Not found in either → return early (unknown price level; can happen on replay gaps, same as existing behaviour).

### Asks

Mirror of bids: promote `overflow_asks_.begin()` (lowest overflow ask) on depletion.

---

## Read Path

### top_of_book()

```cpp
// Best bid = flat_bids_.data[flat_bids_.size - 1]  (no map access)
// Best ask = flat_asks_.data[0]                     (no map access)
```

Returns immediately from flat array. Only falls back to overflow if flat array is empty (pre-open or extreme market conditions).

### bid_levels(n) / ask_levels(n)

1. Copy up to `min(n, flat_bids_.size)` entries from flat array (descending order for bids).
2. If `n > flat_bids_.size`, append from `overflow_bids_` (descending iterator) until `n` levels collected.

---

## Correctness Invariants

At all times:
- Every price in `flat_bids_` is strictly greater than every price in `overflow_bids_`.
- Every price in `flat_asks_` is strictly less than every price in `overflow_asks_`.
- `flat_bids_.size <= TOP_LEVELS`, `flat_asks_.size <= TOP_LEVELS`.
- `overflow_*` maps contain prices only when flat arrays are full.

These invariants must hold after every `add_to_level` and `remove_from_level` call.

---

## Files Changed

| File | Change |
|------|--------|
| `include/book/order_book.h` | Add `PriceLevel`, `FlatLevels`, `TOP_LEVELS`; replace `bids_`/`asks_` with four new members |
| `src/book/order_book.cpp` | Rewrite `add_to_level`, `remove_from_level`; update `top_of_book`, `bid_levels`, `ask_levels` |

No other files change.

---

## Benchmark Plan

After implementation, run:
```bash
cmake --build build --target lob_bench && \
./build/lob_bench --benchmark_min_time=2s --benchmark_repetitions=3 \
                  --benchmark_report_aggregates_only=true
```

Update the baseline comment blocks in `bench/bench_book.cpp` and `bench/bench_pipeline.cpp` with the new numbers. Report before/after delta for `BM_AddOrder`, `BM_DeleteOrder`, `BM_FullPipeline`.

**Note on benchmark fixture vs reality:** The benchmark fixture pre-fills 200 price levels per side, so the flat array (size 20) will be full and overflow will be active from the start. This tests the demotion/promotion path under load. In real AAPL data with ~10–30 active levels, the flat array will almost never overflow — so real-world improvement will likely exceed the benchmark improvement.
