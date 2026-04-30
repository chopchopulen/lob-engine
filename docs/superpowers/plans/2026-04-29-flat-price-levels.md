# Flat Price Level Array Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `std::map` price level storage in `OrderBook` with a hybrid flat sorted array (top 20 levels) + overflow `std::map`, improving cache locality for the common case.

**Architecture:** A `FlatLevels` struct holds a fixed `std::array<PriceLevel, 20>` sorted ascending by price. For bids, the best is at `data[size-1]`; for asks at `data[0]`. When the flat array is full and a better price arrives, the worst flat level is demoted to an overflow `std::map`. When a flat level is depleted, the best overflow level is promoted back. All public `OrderBook` methods are unchanged.

**Tech Stack:** C++17, `std::array`, `std::map` (overflow only), `assert`-based test binary

---

## File Map

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `test/test_order_book.cpp` | Correctness tests via public API |
| Modify | `CMakeLists.txt` | Add `lob_test` target |
| Modify | `include/book/order_book.h` | Add `PriceLevel`, `FlatLevels`, `TOP_LEVELS`; swap private members |
| Modify | `src/book/order_book.cpp` | Rewrite `add_to_level`, `remove_from_level`; update reads |

---

## Task 1: Write correctness tests + CMake target

Tests are written against the public API only. They compile and pass with the current `std::map` implementation — this confirms the tests correctly specify the contract before we touch a single line of book logic.

**Files:**
- Create: `test/test_order_book.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create test/test_order_book.cpp**

```cpp
#include "book/order_book.h"
#include <cassert>
#include <iostream>

// ── test_basic_top_of_book ────────────────────────────────────────────────────
// A few orders on each side; verify best bid and best ask.
static void test_basic_top_of_book() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9800);
    book.add_order(3, 3, 'S', 150, 10000);
    book.add_order(4, 4, 'S', 100, 10100);

    auto snap = book.top_of_book(5);
    assert(snap.best_bid_price  == 9900);
    assert(snap.best_bid_shares == 100);
    assert(snap.best_ask_price  == 10000);
    assert(snap.best_ask_shares == 150);
    std::cout << "PASS test_basic_top_of_book\n";
}

// ── test_bid_levels_ordering ──────────────────────────────────────────────────
// bid_levels() must return levels descending (best bid first = highest price).
static void test_bid_levels_ordering() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9800);
    book.add_order(3, 3, 'B', 300, 9850);

    auto levels = book.bid_levels(3);
    assert(levels.size() == 3);
    assert(levels[0].first == 9900);
    assert(levels[1].first == 9850);
    assert(levels[2].first == 9800);
    std::cout << "PASS test_bid_levels_ordering\n";
}

// ── test_ask_levels_ordering ──────────────────────────────────────────────────
// ask_levels() must return levels ascending (best ask first = lowest price).
static void test_ask_levels_ordering() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'S', 100, 10000);
    book.add_order(2, 2, 'S', 200, 10200);
    book.add_order(3, 3, 'S', 300, 10100);

    auto levels = book.ask_levels(3);
    assert(levels.size() == 3);
    assert(levels[0].first == 10000);
    assert(levels[1].first == 10100);
    assert(levels[2].first == 10200);
    std::cout << "PASS test_ask_levels_ordering\n";
}

// ── test_multiple_orders_same_level ──────────────────────────────────────────
// Several orders at the same price aggregate into one level.
// Deleting one order reduces shares but keeps the level alive.
static void test_multiple_orders_same_level() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9900);
    book.add_order(3, 3, 'B', 150, 9900);

    auto snap = book.top_of_book(4);
    assert(snap.best_bid_price  == 9900);
    assert(snap.best_bid_shares == 450);  // 100+200+150

    book.delete_order(5, 2);  // remove the 200-share order
    auto snap2 = book.top_of_book(6);
    assert(snap2.best_bid_price  == 9900);
    assert(snap2.best_bid_shares == 250);  // 450-200
    std::cout << "PASS test_multiple_orders_same_level\n";
}

// ── test_delete_removes_level ─────────────────────────────────────────────────
// Deleting the last order at a price removes the entire level.
static void test_delete_removes_level() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9800);
    book.delete_order(3, 1);  // remove the only order at 9900

    auto snap = book.top_of_book(4);
    assert(snap.best_bid_price == 9800);  // 9900 is gone

    auto levels = book.bid_levels(5);
    assert(levels.size() == 1);
    assert(levels[0].first == 9800);
    std::cout << "PASS test_delete_removes_level\n";
}

// ── test_overflow_and_promotion_bids ─────────────────────────────────────────
// Add TOP_LEVELS+1 = 21 bid levels. The 21st (worst) should overflow.
// Then delete the best flat level → the overflow level gets promoted.
//
// Orders: ref i → price 9800+(i-1), i=1..21
//   refs 2..21 → prices 9801..9820 → in flat array (top 20)
//   ref 1      → price 9800        → in overflow
static void test_overflow_and_promotion_bids() {
    OrderBook book("TEST");
    for (int i = 1; i <= 21; ++i)
        book.add_order(i, i, 'B', 100, 9800 + (i - 1));

    // All 21 levels are accessible
    auto levels = book.bid_levels(21);
    assert(levels.size() == 21);
    assert(levels[0].first  == 9820);  // best
    assert(levels[20].first == 9800);  // worst (in overflow)

    // Delete the best flat level (9820, ref=21)
    book.delete_order(100, 21);

    auto snap = book.top_of_book(101);
    assert(snap.best_bid_price == 9819);  // 9820 gone; next best is 9819

    // After promotion, 9800 must still be reachable in bid_levels(20)
    auto levels2 = book.bid_levels(20);
    assert(levels2.size() == 20);
    assert(levels2.back().first == 9800);  // promoted from overflow
    std::cout << "PASS test_overflow_and_promotion_bids\n";
}

// ── test_overflow_and_promotion_asks ─────────────────────────────────────────
// Mirror of the bid test for the ask side.
// Orders: ref i → price 10000+(i-1), i=1..21
//   refs 1..20 → prices 10000..10019 → in flat array (top 20, lowest)
//   ref 21     → price 10020         → in overflow
static void test_overflow_and_promotion_asks() {
    OrderBook book("TEST");
    for (int i = 1; i <= 21; ++i)
        book.add_order(i, i, 'S', 100, 10000 + (i - 1));

    auto levels = book.ask_levels(21);
    assert(levels.size() == 21);
    assert(levels[0].first  == 10000);  // best ask
    assert(levels[20].first == 10020);  // worst (in overflow)

    // Delete the best flat level (10000, ref=1)
    book.delete_order(100, 1);

    auto snap = book.top_of_book(101);
    assert(snap.best_ask_price == 10001);

    auto levels2 = book.ask_levels(20);
    assert(levels2.size() == 20);
    assert(levels2.back().first == 10020);  // promoted from overflow
    std::cout << "PASS test_overflow_and_promotion_asks\n";
}

// ── test_execute_order ────────────────────────────────────────────────────────
// Partial fill reduces shares. Full fill removes the level entirely.
static void test_execute_order() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'S', 100, 10000);

    book.execute_order(2, 1, 50);  // partial fill
    auto snap = book.top_of_book(3);
    assert(snap.best_ask_price  == 10000);
    assert(snap.best_ask_shares == 50);

    book.execute_order(4, 1, 50);  // full fill — level vanishes
    auto snap2 = book.top_of_book(5);
    assert(snap2.best_ask_price == 0);  // no asks left
    std::cout << "PASS test_execute_order\n";
}

// ── test_replace_order ────────────────────────────────────────────────────────
// Replace moves the order to a new price with new quantity.
static void test_replace_order() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.replace_order(2, 1, 2, 200, 9950);  // old_ref=1 → new_ref=2, price=9950, qty=200

    auto snap = book.top_of_book(3);
    assert(snap.best_bid_price  == 9950);
    assert(snap.best_bid_shares == 200);

    // Old level must be gone
    auto levels = book.bid_levels(5);
    assert(levels.size() == 1);
    assert(levels[0].first == 9950);
    std::cout << "PASS test_replace_order\n";
}

int main() {
    test_basic_top_of_book();
    test_bid_levels_ordering();
    test_ask_levels_ordering();
    test_multiple_orders_same_level();
    test_delete_removes_level();
    test_overflow_and_promotion_bids();
    test_overflow_and_promotion_asks();
    test_execute_order();
    test_replace_order();
    std::cout << "\nAll 9 tests passed.\n";
    return 0;
}
```

- [ ] **Step 2: Add lob_test target to CMakeLists.txt**

Append after the `lob_bench` block:

```cmake
# ── Correctness tests ─────────────────────────────────────────────────────────
add_executable(lob_test
    test/test_order_book.cpp
    src/book/order_book.cpp
    src/feed/itch_parser.cpp
    src/features/feature_engine.cpp
)
target_include_directories(lob_test PRIVATE include)
```

- [ ] **Step 3: Configure and build against current (std::map) implementation**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target lob_test 2>&1
```

Expected: clean build. The test binary compiles because the public API is unchanged.

- [ ] **Step 4: Run tests against current implementation (establish baseline)**

```bash
./build/lob_test
```

Expected output:
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

All 9 tests must pass before touching a single line of book logic. If any fail here, fix the test — not the implementation.

---

## Task 2: Update order_book.h and rewrite order_book.cpp

Header and implementation must change atomically — updating the header without the `.cpp` leaves dangling references to `bids_` and `asks_`.

**Files:**
- Modify: `include/book/order_book.h`
- Modify: `src/book/order_book.cpp`

- [ ] **Step 1: Replace include/book/order_book.h**

Replace the entire file with:

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// OrderBook: reconstructs the full limit order book from ITCH messages.
//
// WHAT A LIMIT ORDER BOOK IS:
//   A limit order book (LOB) is the exchange's real-time record of all
//   outstanding buy and sell orders, organized by price level.
//
//   Buy orders (bids) stack up below the current price.
//   Sell orders (asks) stack up above the current price.
//   The "spread" is the gap between the best bid and best ask.
//
// HOW WE RECONSTRUCT IT:
//   We start with an empty book. For each ITCH message:
//   - AddOrder:  record the order's price, size, and side; add to that price level
//   - DeleteOrder: remove the order from its price level
//   - ReplaceOrder: delete old, insert new (at possibly different price/qty)
//   - ExecuteOrder: reduce the order's remaining shares; if 0, remove it
//
// DATA STRUCTURES:
//   orders_: unordered_map<order_ref → Order>
//     Fast O(1) lookup by order ID (needed for cancel/execute)
//
//   flat_bids_ / flat_asks_: FlatLevels (sorted array, size ≤ TOP_LEVELS)
//     The top TOP_LEVELS price levels closest to the spread.
//     Sorted ascending by price; best bid = data[size-1], best ask = data[0].
//     Fits in a cache line or two — no pointer chasing for the hot path.
//
//   overflow_bids_ / overflow_asks_: std::map<price → shares>
//     Price levels beyond the top TOP_LEVELS. Rarely accessed in practice.
// ─────────────────────────────────────────────────────────────────────────────

// Number of price levels kept in the flat array (compile-time constant).
// The compiler can unroll inner loops over this range.
static constexpr uint32_t TOP_LEVELS = 20;

struct PriceLevel {
    uint32_t price;
    uint32_t shares;
};

// Flat sorted array of up to TOP_LEVELS price levels.
// Always sorted ascending by price (index 0 = lowest price).
//   Best bid  = data[size-1]  (highest price)
//   Best ask  = data[0]       (lowest price)
struct FlatLevels {
    std::array<PriceLevel, TOP_LEVELS> data{};
    uint32_t size = 0;
};

struct Order {
    uint64_t order_ref;
    char     side;    // 'B' or 'S'
    uint32_t price;   // in 1/10000 dollars
    uint32_t shares;  // remaining shares
};

// A snapshot of the top-of-book state at a point in time
struct BookSnapshot {
    uint64_t timestamp_ns;
    uint32_t best_bid_price;   // 0 if no bids
    uint32_t best_bid_shares;
    uint32_t best_ask_price;   // 0 if no asks
    uint32_t best_ask_shares;

    double mid_price() const {
        if (best_bid_price == 0 || best_ask_price == 0) return 0.0;
        return (best_bid_price + best_ask_price) / 2.0 / 10000.0;
    }

    double spread() const {
        if (best_bid_price == 0 || best_ask_price == 0) return 0.0;
        return (best_ask_price - best_bid_price) / 10000.0;
    }
};

class OrderBook {
public:
    explicit OrderBook(const std::string& symbol) : symbol_(symbol) {}

    void add_order(uint64_t ts, uint64_t order_ref, char side,
                   uint32_t shares, uint32_t price);
    void delete_order(uint64_t ts, uint64_t order_ref);
    void replace_order(uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                       uint32_t new_shares, uint32_t new_price);
    void execute_order(uint64_t ts, uint64_t order_ref, uint32_t executed_shares);

    BookSnapshot top_of_book(uint64_t ts) const;

    std::vector<std::pair<uint32_t, uint32_t>> bid_levels(int n) const;
    std::vector<std::pair<uint32_t, uint32_t>> ask_levels(int n) const;

    size_t num_orders() const { return orders_.size(); }
    const std::string& symbol() const { return symbol_; }

private:
    std::string symbol_;
    uint64_t    last_ts_ = 0;

    std::unordered_map<uint64_t, Order> orders_;

    FlatLevels                   flat_bids_;      // top TOP_LEVELS bid prices
    FlatLevels                   flat_asks_;      // top TOP_LEVELS ask prices
    std::map<uint32_t, uint32_t> overflow_bids_;  // bids below the top TOP_LEVELS
    std::map<uint32_t, uint32_t> overflow_asks_;  // asks above the top TOP_LEVELS

    void add_to_level(char side, uint32_t price, uint32_t shares);
    void remove_from_level(char side, uint32_t price, uint32_t shares);
};
```

- [ ] **Step 2: Replace src/book/order_book.cpp**

Replace the entire file with:

```cpp
#include "book/order_book.h"
#include <algorithm>

// ── Flat-array helpers ────────────────────────────────────────────────────────
// These operate on FlatLevels (always sorted ascending by price).
// With TOP_LEVELS=20 and -O2, the compiler fully unrolls the scan loops.

// Return index of price in flat array, or -1 if not found.
static int flat_find(const FlatLevels& f, uint32_t price) {
    for (uint32_t i = 0; i < f.size; ++i)
        if (f.data[i].price == price) return (int)i;
    return -1;
}

// Insert {price, shares} maintaining ascending sort.
// Precondition: f.size < TOP_LEVELS.
static void flat_insert(FlatLevels& f, uint32_t price, uint32_t shares) {
    uint32_t i = f.size;
    while (i > 0 && f.data[i - 1].price > price) {
        f.data[i] = f.data[i - 1];
        --i;
    }
    f.data[i] = {price, shares};
    ++f.size;
}

// Remove element at idx (shift left to close the gap).
static void flat_remove_at(FlatLevels& f, uint32_t idx) {
    for (uint32_t i = idx + 1; i < f.size; ++i)
        f.data[i - 1] = f.data[i];
    --f.size;
}

// ── add_to_level ──────────────────────────────────────────────────────────────
//
// For bids (highest prices are best):
//   Flat array sorted ascending → worst bid = data[0], best bid = data[size-1].
//   A new bid is "better than the worst" if price > data[0].price.
//
// For asks (lowest prices are best):
//   Flat array sorted ascending → best ask = data[0], worst ask = data[size-1].
//   A new ask is "better than the worst" if price < data[size-1].price.

void OrderBook::add_to_level(char side, uint32_t price, uint32_t shares) {
    FlatLevels&                   flat     = (side == 'B') ? flat_bids_     : flat_asks_;
    std::map<uint32_t, uint32_t>& overflow = (side == 'B') ? overflow_bids_ : overflow_asks_;

    // Case 1: price already in flat array → update shares in place (no structural change)
    int idx = flat_find(flat, price);
    if (idx >= 0) {
        flat.data[idx].shares += shares;
        return;
    }

    // Case 2: price already in overflow → update there
    auto ov = overflow.find(price);
    if (ov != overflow.end()) {
        ov->second += shares;
        return;
    }

    // Case 3: brand-new price level
    if (flat.size < TOP_LEVELS) {
        // Flat array has room — insert directly
        flat_insert(flat, price, shares);
        return;
    }

    // Flat array is full. Check if the new price beats the worst flat level.
    if (side == 'B') {
        // Worst flat bid = data[0] (lowest price in flat array)
        if (price > flat.data[0].price) {
            overflow[flat.data[0].price] = flat.data[0].shares;  // demote worst
            flat_remove_at(flat, 0);
            flat_insert(flat, price, shares);
        } else {
            overflow[price] = shares;  // new bid is worse than everything in flat
        }
    } else {
        // Worst flat ask = data[size-1] (highest price in flat array)
        if (price < flat.data[flat.size - 1].price) {
            overflow[flat.data[flat.size - 1].price] = flat.data[flat.size - 1].shares;
            flat_remove_at(flat, flat.size - 1);
            flat_insert(flat, price, shares);
        } else {
            overflow[price] = shares;
        }
    }
}

// ── remove_from_level ─────────────────────────────────────────────────────────

void OrderBook::remove_from_level(char side, uint32_t price, uint32_t shares) {
    FlatLevels&                   flat     = (side == 'B') ? flat_bids_     : flat_asks_;
    std::map<uint32_t, uint32_t>& overflow = (side == 'B') ? overflow_bids_ : overflow_asks_;

    // Try flat array first
    int idx = flat_find(flat, price);
    if (idx >= 0) {
        if (flat.data[idx].shares <= shares) {
            // Level fully consumed — remove and promote from overflow if possible
            flat_remove_at(flat, (uint32_t)idx);
            if (!overflow.empty()) {
                if (side == 'B') {
                    // Promote highest overflow bid (best of the rest)
                    auto it = overflow.end(); --it;
                    flat_insert(flat, it->first, it->second);
                    overflow.erase(it);
                } else {
                    // Promote lowest overflow ask
                    auto it = overflow.begin();
                    flat_insert(flat, it->first, it->second);
                    overflow.erase(it);
                }
            }
        } else {
            flat.data[idx].shares -= shares;
        }
        return;
    }

    // Try overflow map
    auto it = overflow.find(price);
    if (it == overflow.end()) return;  // unknown level — replay gap, silently skip

    if (it->second <= shares) {
        overflow.erase(it);
    } else {
        it->second -= shares;
    }
}

// ── Public order operations ───────────────────────────────────────────────────

void OrderBook::add_order(uint64_t ts, uint64_t order_ref, char side,
                           uint32_t shares, uint32_t price) {
    last_ts_ = ts;
    orders_[order_ref] = Order{order_ref, side, price, shares};
    add_to_level(side, price, shares);
}

void OrderBook::delete_order(uint64_t ts, uint64_t order_ref) {
    last_ts_ = ts;
    auto it = orders_.find(order_ref);
    if (it == orders_.end()) return;
    const Order& o = it->second;
    remove_from_level(o.side, o.price, o.shares);
    orders_.erase(it);
}

void OrderBook::replace_order(uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                               uint32_t new_shares, uint32_t new_price) {
    last_ts_ = ts;
    auto it = orders_.find(old_ref);
    if (it == orders_.end()) return;
    const Order old = it->second;
    remove_from_level(old.side, old.price, old.shares);
    orders_.erase(it);
    orders_[new_ref] = Order{new_ref, old.side, new_price, new_shares};
    add_to_level(old.side, new_price, new_shares);
}

void OrderBook::execute_order(uint64_t ts, uint64_t order_ref,
                               uint32_t executed_shares) {
    last_ts_ = ts;
    auto it = orders_.find(order_ref);
    if (it == orders_.end()) return;
    Order& o = it->second;
    uint32_t filled = std::min(executed_shares, o.shares);
    remove_from_level(o.side, o.price, filled);
    o.shares -= filled;
    if (o.shares == 0) orders_.erase(it);
}

// ── Queries ───────────────────────────────────────────────────────────────────

BookSnapshot OrderBook::top_of_book(uint64_t ts) const {
    BookSnapshot snap{};
    snap.timestamp_ns = ts;
    // Best bid = highest price in flat_bids_ = last element (sorted ascending)
    if (flat_bids_.size > 0) {
        const auto& b     = flat_bids_.data[flat_bids_.size - 1];
        snap.best_bid_price  = b.price;
        snap.best_bid_shares = b.shares;
    }
    // Best ask = lowest price in flat_asks_ = first element
    if (flat_asks_.size > 0) {
        const auto& a     = flat_asks_.data[0];
        snap.best_ask_price  = a.price;
        snap.best_ask_shares = a.shares;
    }
    return snap;
}

std::vector<std::pair<uint32_t, uint32_t>> OrderBook::bid_levels(int n) const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    result.reserve(n);
    // Flat array: walk backwards (descending price = best bid first)
    int flat_take = std::min(n, (int)flat_bids_.size);
    for (int i = (int)flat_bids_.size - 1; i >= (int)flat_bids_.size - flat_take; --i)
        result.push_back({flat_bids_.data[i].price, flat_bids_.data[i].shares});
    // Overflow: also descending
    int rem = n - flat_take;
    for (auto it = overflow_bids_.rbegin(); it != overflow_bids_.rend() && rem > 0; ++it, --rem)
        result.push_back({it->first, it->second});
    return result;
}

std::vector<std::pair<uint32_t, uint32_t>> OrderBook::ask_levels(int n) const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    result.reserve(n);
    // Flat array: walk forwards (ascending price = best ask first)
    int flat_take = std::min(n, (int)flat_asks_.size);
    for (int i = 0; i < flat_take; ++i)
        result.push_back({flat_asks_.data[i].price, flat_asks_.data[i].shares});
    // Overflow: also ascending
    int rem = n - flat_take;
    for (auto it = overflow_asks_.begin(); it != overflow_asks_.end() && rem > 0; ++it, --rem)
        result.push_back({it->first, it->second});
    return result;
}
```

---

## Task 3: Build everything and run tests

**Files:** none — build and test only

- [ ] **Step 1: Build all targets**

```bash
cmake --build build 2>&1
```

Expected: all three targets (`lob_engine`, `lob_bench`, `lob_test`) compile cleanly. Common errors and fixes:
- `'bids_' was not declared` → you still have old member references in order_book.cpp; check Task 2 Step 2 was applied fully
- `no member named 'data' in 'FlatLevels'` → `#include <array>` missing from order_book.h; check Task 2 Step 1
- `static_assert` or template errors from `<array>` → ensure C++17 is set (already is in CMakeLists.txt)

- [ ] **Step 2: Run correctness tests**

```bash
./build/lob_test
```

Expected:
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

If any test fails, diagnose using the test name:
- `test_overflow_and_promotion_bids` fails → demotion or promotion logic for bids is wrong. Invariant: every price in `flat_bids_` must be strictly greater than every price in `overflow_bids_`.
- `test_overflow_and_promotion_asks` fails → mirror issue for asks. Invariant: every price in `flat_asks_` must be strictly less than every price in `overflow_asks_`.
- `test_bid_levels_ordering` fails → `bid_levels()` is not walking `flat_bids_` in descending order. Check the loop in `bid_levels()` walks from `size-1` down to `0`.
- `test_basic_top_of_book` fails → `top_of_book()` is reading the wrong index. Best bid = `flat_bids_.data[flat_bids_.size - 1]`; best ask = `flat_asks_.data[0]`.

Do NOT proceed to Task 4 until all 9 tests pass.

---

## Task 4: Run benchmarks and record before/after

**Files:**
- Modify: `bench/bench_book.cpp` (update baseline comment)
- Modify: `bench/bench_pipeline.cpp` (update baseline comment)

- [ ] **Step 1: Run all benchmarks**

```bash
./build/lob_bench \
  --benchmark_min_time=2s \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

- [ ] **Step 2: Apply sanity checks to the output**

The benchmark fixture pre-fills 200 price levels per side, so the flat array (TOP_LEVELS=20) is full from the start and overflow is active. This is a stress test of the demotion/promotion path — the improvement in real usage will be larger.

Sanity checks:
- `BM_FullPipeline` p50 should be ≤ the `std::map` baseline (42ns). If it's higher, there's a regression — re-check `add_to_level` and `top_of_book` for unnecessary work.
- P99/P50 ratio should remain between 1× and 10× for all benchmarks.
- `BM_ReplaceOrder` should remain roughly equal to or higher than `BM_AddOrder` (it does both a remove and an insert).

- [ ] **Step 3: Update baseline comments in bench/bench_book.cpp**

Replace the existing baseline comment block (lines 8–13) with the new numbers:

```cpp
// ── Baseline comparison (Apple Silicon, Release -O2) ──────────────────────────
// std::map:   BM_AddOrder p50=41ns p99=42ns | BM_DeleteOrder p50=42ns p99=83ns
// FlatLevels: BM_AddOrder p50=XXns p99=XXns | BM_DeleteOrder p50=XXns p99=XXns
// These are the "after" numbers for roadmap item 6 (flat array vs std::map).
```

- [ ] **Step 4: Update baseline comment in bench/bench_pipeline.cpp**

Replace the existing baseline comment block (lines 8–10) with:

```cpp
// ── Baseline comparison (Apple Silicon, Release -O2) ──────────────────────────
// std::map:   BM_FullPipeline p50=42ns p99=84ns
// FlatLevels: BM_FullPipeline p50=XXns p99=XXns
```

- [ ] **Step 5: Build lob_engine to confirm main pipeline still works**

```bash
cmake --build build --target lob_engine 2>&1
```

Expected: clean build. The main pipeline is unchanged — `FeatureEngine`, `ItchParser`, and `main.cpp` require no modification.
