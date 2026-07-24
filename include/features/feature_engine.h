#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include "book/order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// FeatureEngine: computes microstructure features from the order book.
//
// These are the same features used in academic research and at HFT firms
// to understand short-term price dynamics.
//
// FEATURES:
//
// 1. ORDER FLOW IMBALANCE (OFI) — Cont, Kukanov & Stoikov (2014)
//    The central microstructure signal. Measures the net pressure on price
//    from order book *changes* (not levels). Specifically:
//      - Best bid increases in size OR price → positive pressure (+OFI)
//      - Best ask decreases in size OR price → positive pressure (+OFI)
//      - Best bid decreases in size OR price → negative pressure (-OFI)
//      - Best ask increases in size OR price → negative pressure (-OFI)
//    OFI predicts short-term returns with R² of 5-15% on liquid stocks.
//
// 2. VOLUME-WEIGHTED MICRO-PRICE
//    Better fair-value estimate than simple mid-price.
//    micro_price = (bid_price × ask_size + ask_price × bid_size)
//                  / (bid_size + ask_size)
//    Intuitively: weighted towards whichever side has less liquidity
//    (because that side is under more pressure).
//
// 3. QUOTED SPREAD
//    ask_price - bid_price, in dollars.
//    Proxy for transaction cost and adverse selection risk.
//
// 4. TRADE IMBALANCE (simplified VPIN proxy)
//    Volume of buyer-initiated trades minus seller-initiated trades,
//    over a rolling window. High imbalance → informed trading pressure.
// ─────────────────────────────────────────────────────────────────────────────

struct FeatureRow {
    uint64_t timestamp_ns;
    double   best_bid;         // raw best bid price, dollars — straight from BookSnapshot,
    double   best_ask;         // NOT re-derived from mid_price/quoted_spread. Validation
                                // must check these directly: reconstructing bid/ask from
                                // mid +- spread/2 can pass a row whose mid_price or
                                // quoted_spread is itself wrong, since that reconstruction
                                // is definitionally self-consistent with them.
    double   mid_price;       // (bid + ask) / 2
    double   micro_price;     // volume-weighted fair value
    double   quoted_spread;   // ask - bid in dollars
    double   ofi;             // order flow imbalance (per Cont et al.)
    double   normalized_ofi;  // ofi / avg_trade_size — dimensionless, cross-instrument comparable
    double   trade_imbalance; // buy volume - sell volume (rolling window)

    // Multi-level OFI (for depth analysis — Levels 1, 5, 10)
    double   ofi_l1;
    double   ofi_l5;
    double   ofi_l10;
};

class FeatureEngine {
public:
    // Call this on every book update — it records the new state
    // and computes features if enough time has passed.
    // Returns a completed FeatureRow (or nullopt if not ready).
    void on_book_update(const OrderBook& book, uint64_t ts);

    // Call this when a trade executes, so we can track buy/sell volume
    void on_trade(char aggressor_side, uint32_t shares, uint64_t ts);

    // Get all completed feature rows
    const std::vector<FeatureRow>& rows() const { return rows_; }

    // Write all rows to a CSV file
    void write_csv(const std::string& path) const;

private:
    // ── Tick-by-tick OFI accumulation ────────────────────────────────────────
    // prev_tick_snap_: book state after the LAST message (updated every message)
    // running_ofi_:    sum of all OFI deltas within the current 1-second window
    //
    // Every message: delta = compute_ofi(prev_tick_snap_, curr)
    //                running_ofi_ += delta
    //                prev_tick_snap_ = curr
    //
    // At second boundary: report running_ofi_, reset to 0
    //
    // This is the correct implementation of Cont et al. (2014).
    // The old approach (snapshot every second) threw away all intra-second info.
    BookSnapshot prev_tick_snap_{};
    bool         has_prev_tick_ = false;
    double       running_ofi_   = 0.0;

    // Previous SECOND-level snapshot (for the snapshot row's book state)
    BookSnapshot prev_second_snap_{};
    bool         has_prev_second_ = false;

    // Rolling trade volume for imbalance computation (last 10 seconds)
    struct TradeEvent { uint64_t ts; char side; uint32_t shares; };
    std::deque<TradeEvent> trade_window_;
    uint64_t trade_window_ns_ = 10ULL * 1'000'000'000ULL;

    // Rolling executed shares for OFI normalization (last 60 seconds)
    // avg_trade_size = total_shares / count; normalized_ofi = ofi / avg_trade_size
    struct NormEvent { uint64_t ts; uint32_t shares; };
    std::deque<NormEvent> ofi_norm_window_;
    uint64_t ofi_norm_window_ns_ = 60ULL * 1'000'000'000ULL;

    // Completed feature rows
    std::vector<FeatureRow> rows_;

    // Interval between feature snapshots (1 second = 1e9 ns)
    uint64_t snapshot_interval_ns_ = 1'000'000'000ULL;
    uint64_t last_snapshot_ns_     = 0;

    // Compute single OFI delta between two consecutive book states
    double compute_ofi(const BookSnapshot& prev, const BookSnapshot& curr) const;
};
