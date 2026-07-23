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
            overflow[price] += shares;  // new bid is worse than everything in flat
        }
    } else {
        // Worst flat ask = data[size-1] (highest price in flat array)
        if (price < flat.data[flat.size - 1].price) {
            overflow[flat.data[flat.size - 1].price] = flat.data[flat.size - 1].shares;
            flat_remove_at(flat, flat.size - 1);
            flat_insert(flat, price, shares);
        } else {
            overflow[price] += shares;
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
