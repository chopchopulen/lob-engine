#include "features/feature_engine.h"
#include <fstream>
#include <cmath>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
// OFI computation — the key formula from Cont et al. (2014)
//
// For each update, we compare the new best bid/ask to the previous one:
//
//   e_bid(t) = +Q_bid(t)  if P_bid(t) > P_bid(t-1)   [bid price improved]
//            = +Q_bid(t) - Q_bid(t-1)  if P_bid(t) == P_bid(t-1)  [same price, size changed]
//            = -Q_bid(t-1)  if P_bid(t) < P_bid(t-1)  [bid price dropped]
//
//   e_ask(t) = -Q_ask(t)  if P_ask(t) > P_ask(t-1)   [ask price rose = less pressure]
//            = Q_ask(t-1) - Q_ask(t)  if P_ask(t) == P_ask(t-1)
//            = +Q_ask(t-1)  if P_ask(t) < P_ask(t-1)  [ask price improved for buyers]
//
//   OFI(t) = e_bid(t) - e_ask(t)
//
// Positive OFI → more buying pressure → price likely to go up
// Negative OFI → more selling pressure → price likely to go down
// ─────────────────────────────────────────────────────────────────────────────

double FeatureEngine::compute_ofi(const BookSnapshot& prev,
                                   const BookSnapshot& curr) const {
    double e_bid = 0.0;
    double e_ask = 0.0;

    // Bid side contribution
    if (curr.best_bid_price > prev.best_bid_price) {
        e_bid = curr.best_bid_shares;
    } else if (curr.best_bid_price == prev.best_bid_price) {
        e_bid = (double)curr.best_bid_shares - (double)prev.best_bid_shares;
    } else {
        e_bid = -(double)prev.best_bid_shares;
    }

    // Ask side contribution (sign flipped: rising ask = selling pressure)
    if (curr.best_ask_price < prev.best_ask_price) {
        e_ask = curr.best_ask_shares;
    } else if (curr.best_ask_price == prev.best_ask_price) {
        e_ask = (double)curr.best_ask_shares - (double)prev.best_ask_shares;
    } else {
        e_ask = -(double)prev.best_ask_shares;
    }

    return e_bid - e_ask;
}

// ─────────────────────────────────────────────────────────────────────────────
// Trade recording — needed for imbalance feature
// ─────────────────────────────────────────────────────────────────────────────

void FeatureEngine::on_trade(char aggressor_side, uint32_t shares, uint64_t ts) {
    trade_window_.push_back({ts, aggressor_side, shares});
    while (!trade_window_.empty() &&
           (ts - trade_window_.front().ts) > trade_window_ns_) {
        trade_window_.pop_front();
    }

    ofi_norm_window_.push_back({ts, shares});
    while (!ofi_norm_window_.empty() &&
           (ts - ofi_norm_window_.front().ts) > ofi_norm_window_ns_) {
        ofi_norm_window_.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Book update — called after every ITCH message is applied to the book
// ─────────────────────────────────────────────────────────────────────────────

void FeatureEngine::on_book_update(const OrderBook& book, uint64_t ts) {
    BookSnapshot curr = book.top_of_book(ts);

    // Skip if no valid top of book yet (e.g. pre-open, empty book)
    if (curr.best_bid_price == 0 || curr.best_ask_price == 0) {
        return;
    }

    // ── Step 1: accumulate OFI delta for THIS message ─────────────────────
    // This runs on every single message — hundreds of millions of times.
    // Each call adds a tiny delta to running_ofi_.
    // By the time a second has passed, running_ofi_ contains the TRUE OFI
    // for that second: the sum of every single book change's pressure signal.
    if (has_prev_tick_) {
        running_ofi_ += compute_ofi(prev_tick_snap_, curr);
    }
    prev_tick_snap_ = curr;
    has_prev_tick_  = true;

    // ── Step 2: emit a feature row once per second ────────────────────────
    // Only crosses this threshold ~55,000 times in a full trading day.
    if (last_snapshot_ns_ > 0 &&
        ts - last_snapshot_ns_ < snapshot_interval_ns_) {
        return;
    }
    last_snapshot_ns_ = ts;

    // ── Compute the rest of the features using current book state ──────────

    FeatureRow row{};
    row.timestamp_ns  = ts;
    row.mid_price     = curr.mid_price();
    row.quoted_spread = curr.spread();

    // Volume-weighted micro-price
    // Weighted toward whichever side has LESS liquidity — that side is
    // under more pressure, so fair value should be closer to it.
    double bid_p = curr.best_bid_price / 10000.0;
    double ask_p = curr.best_ask_price / 10000.0;
    row.best_bid = bid_p;   // raw, straight from the book snapshot — see FeatureRow comment
    row.best_ask = ask_p;
    double bid_q = curr.best_bid_shares;
    double ask_q = curr.best_ask_shares;
    double tot   = bid_q + ask_q;
    row.micro_price = (tot > 0)
        ? (bid_p * ask_q + ask_p * bid_q) / tot
        : row.mid_price;

    // OFI: the accumulated sum of all tick-level deltas this second
    row.ofi = running_ofi_;
    running_ofi_ = 0.0;  // reset for the next second

    // Normalized OFI: divide by rolling avg trade size so signal is
    // dimensionless and comparable across instruments (Cont et al. 2014)
    {
        double total_shares = 0.0;
        for (const auto& e : ofi_norm_window_) total_shares += e.shares;
        double avg_trade_size = (ofi_norm_window_.empty())
            ? 0.0
            : total_shares / static_cast<double>(ofi_norm_window_.size());
        row.normalized_ofi = (avg_trade_size > 0.0) ? row.ofi / avg_trade_size : 0.0;
    }

    // Multi-level depth imbalance (bid volume - ask volume at N levels)
    // This answers: does looking deeper into the book add signal?
    auto depth_imbalance = [&](int n) -> double {
        auto bids = book.bid_levels(n);
        auto asks = book.ask_levels(n);
        double bvol = 0, avol = 0;
        for (auto& [p, q] : bids) bvol += q;
        for (auto& [p, q] : asks) avol += q;
        return bvol - avol;
    };
    row.ofi_l1  = depth_imbalance(1);
    row.ofi_l5  = depth_imbalance(5);
    row.ofi_l10 = depth_imbalance(10);

    // Trade imbalance: rolling 10-second buy volume minus sell volume
    double buy_vol = 0, sell_vol = 0;
    for (const auto& t : trade_window_) {
        if (t.side == 'B') buy_vol  += t.shares;
        else               sell_vol += t.shares;
    }
    row.trade_imbalance = buy_vol - sell_vol;

    rows_.push_back(row);
}

// ─────────────────────────────────────────────────────────────────────────────
// CSV export — feeds directly into analysis.py
// ─────────────────────────────────────────────────────────────────────────────

void FeatureEngine::write_csv(const std::string& path) const {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write CSV: " + path);

    f << "ts,best_bid,best_ask,mid_price,micro_price,quoted_spread,ofi,normalized_ofi,"
      << "trade_imbalance,ofi_l1,ofi_l5,ofi_l10\n";

    for (const auto& r : rows_) {
        f << r.timestamp_ns    << ","
          << r.best_bid        << ","
          << r.best_ask        << ","
          << r.mid_price       << ","
          << r.micro_price     << ","
          << r.quoted_spread   << ","
          << r.ofi             << ","
          << r.normalized_ofi  << ","
          << r.trade_imbalance << ","
          << r.ofi_l1          << ","
          << r.ofi_l5          << ","
          << r.ofi_l10         << "\n";
    }
}
