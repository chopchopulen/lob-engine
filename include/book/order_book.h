#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <new>

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

// ─────────────────────────────────────────────────────────────────────────────
// PoolAllocator: fixed-size free-list allocator for node-based containers.
//
// std::unordered_map is node-based: every insert that doesn't hit an
// existing key allocates one heap node, and every erase frees one. On the
// order_ref hot path (add/delete/replace/execute) this happens once or
// twice per ITCH message. This allocator recycles freed nodes via an
// intrusive free-list instead of round-tripping through the general-purpose
// allocator on every op — same alloc/dealloc call sites, no change to
// unordered_map's find/insert/erase semantics or iteration order guarantees.
//
// Single-element (size==1) requests are served from the free-list; anything
// else (e.g. the bucket array, allocated in bulk on rehash) falls back to
// plain ::operator new/delete, since those aren't the hot path this targets.
//
// The free-list is static per instantiated type T, so it's shared across
// all containers using PoolAllocator<T> in the process — intentional: it
// keeps the allocator stateless (trivial equality, no propagate_on_*
// complications) and nodes freed by one OrderBook can be reused by another.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    PoolAllocator() noexcept = default;
    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 1) {
            if (free_list_) {
                Node* node = free_list_;
                free_list_ = free_list_->next;
                return reinterpret_cast<T*>(node);
            }
            return static_cast<T*>(::operator new(sizeof(T)));
        }
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n == 1) {
            Node* node = reinterpret_cast<Node*>(p);
            node->next = free_list_;
            free_list_ = node;
            return;
        }
        ::operator delete(p);
    }

    template <typename U> struct rebind { using other = PoolAllocator<U>; };

    template <typename U>
    bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const PoolAllocator<U>&) const noexcept { return false; }

private:
    union Node {
        Node*                                  next;
        alignas(T) unsigned char               storage[sizeof(T)];
    };
    static inline Node* free_list_ = nullptr;
};

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
    explicit OrderBook(const std::string& symbol) : symbol_(symbol) {
        orders_.reserve(1 << 20);
    }

    void add_order(uint64_t ts, uint64_t order_ref, char side,
                   uint32_t shares, uint32_t price);
    void delete_order(uint64_t ts, uint64_t order_ref);
    void replace_order(uint64_t ts, uint64_t old_ref, uint64_t new_ref,
                       uint32_t new_shares, uint32_t new_price);
    void execute_order(uint64_t ts, uint64_t order_ref, uint32_t executed_shares);
    void cancel_order(uint64_t ts, uint64_t order_ref, uint32_t canceled_shares);

    BookSnapshot top_of_book(uint64_t ts) const;

    // Read-only lookup by order_ref — no mutation, no allocation. Callers
    // needing an order's side/price (e.g. ground-truth trade labeling) must
    // call this BEFORE execute_order()/delete_order(), since a fully-filled
    // or deleted order is erased and no longer found here afterward. Not on
    // any benchmarked call path (see bench/BASELINE.md).
    bool peek_order(uint64_t order_ref, Order& out) const;

    std::vector<std::pair<uint32_t, uint32_t>> bid_levels(int n) const;
    std::vector<std::pair<uint32_t, uint32_t>> ask_levels(int n) const;

    size_t num_orders() const { return orders_.size(); }
    const std::string& symbol() const { return symbol_; }

private:
    std::string symbol_;
    uint64_t    last_ts_ = 0;

    std::unordered_map<uint64_t, Order, std::hash<uint64_t>, std::equal_to<uint64_t>,
                       PoolAllocator<std::pair<const uint64_t, Order>>> orders_;

    FlatLevels                   flat_bids_;      // top TOP_LEVELS bid prices
    FlatLevels                   flat_asks_;      // top TOP_LEVELS ask prices
    std::map<uint32_t, uint32_t> overflow_bids_;  // bids below the top TOP_LEVELS
    std::map<uint32_t, uint32_t> overflow_asks_;  // asks above the top TOP_LEVELS

    void add_to_level(char side, uint32_t price, uint32_t shares);
    void remove_from_level(char side, uint32_t price, uint32_t shares);
};
