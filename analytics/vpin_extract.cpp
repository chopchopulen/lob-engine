#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include "feed/itch_parser.h"
#include "book/order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// vpin_extract — offline analytics tool, NOT part of the benchmarked hot path.
//
// Reprocesses a raw ITCH file for one ticker and writes a per-execution trade
// CSV with ground-truth side (via order-ID linkage, looked up in the live book
// BEFORE the execution mutates it) and the prevailing quote at the moment of
// the trade. Downstream Python (analytics/lee_ready.py, analytics/vpin.py)
// consumes this CSV — this tool only extracts.
//
// Ground truth: for 'E'/'C', the executed order_ref refers to a RESTING order
// already in the book. peek_order() reads its side without mutating. If the
// resting side is 'B' (a passive bid was hit), the aggressor was a SELL; if
// resting side is 'S', the aggressor was a BUY. This is exact, not inferred.
//
// 'P' (hidden/non-displayed) trades get gt_side='U' (unknown) always — per
// TradeMsg's doc comment, Nasdaq has zeroed the order_ref (since 2010-12-06)
// and hardcoded the Buy/Sell Indicator to 'B' (since 2014-07-14), so no
// mechanism can recover their side. They still carry real price/shares.
//
// 'C' executions with printable=='N' are still applied to the book (real
// shares reduction) but flagged printable=N in the CSV — downstream volume/
// bucket code must exclude them or double-count against their later bulk
// print (ITCH 5.0 spec section 1.4.2).
//
// Usage: ./vpin_extract <itch_file> <TICKER> <output_trades.csv>
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <itch_file> <TICKER> <output_trades.csv>\n";
        return 1;
    }
    const std::string itch_file  = argv[1];
    const std::string ticker     = argv[2];
    const std::string output_csv = argv[3];

    auto locate_map = ItchParser::parse_stock_directory(itch_file);
    uint16_t locate = 0;
    bool found = false;
    for (const auto& [loc, sym] : locate_map) {
        if (sym == ticker) { locate = loc; found = true; break; }
    }
    if (!found) {
        std::cerr << "Error: ticker \"" << ticker << "\" not found in Stock Directory.\n";
        return 1;
    }

    std::cout << "vpin_extract\n"
              << "  File:   " << itch_file << "\n"
              << "  Ticker: " << ticker << " (stock_locate=" << locate << ")\n"
              << "  Output: " << output_csv << "\n\n";

    OrderBook book(ticker);
    std::ofstream out(output_csv);
    if (!out) {
        std::cerr << "Error: cannot write " << output_csv << "\n";
        return 1;
    }
    out << "ts,event_type,price,shares,gt_side,resting_side,prevailing_bid,prevailing_ask,printable\n";

    size_t msg_count = 0, trade_count = 0, gt_labeled = 0, non_printable_skipped = 0;

    ParserCallbacks cb;

    cb.on_add = [&](const AddOrderMsg& m) {
        book.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
        ++msg_count;
    };
    cb.on_delete = [&](const DeleteOrderMsg& m) {
        book.delete_order(m.timestamp_ns, m.order_ref);
        ++msg_count;
    };
    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        book.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                            m.new_shares, m.new_price);
        ++msg_count;
    };
    cb.on_cancel = [&](const CancelOrderMsg& m) {
        book.cancel_order(m.timestamp_ns, m.order_ref, m.canceled_shares);
        ++msg_count;
    };
    cb.on_execute = [&](const ExecuteOrderMsg& m) {
        // Ground truth MUST be looked up before execute_order() mutates/erases
        // the resting order.
        Order resting{};
        bool have_resting = book.peek_order(m.order_ref, resting);
        char gt_side = 'U';
        char resting_side_out = 'U';
        if (have_resting) {
            resting_side_out = resting.side;
            gt_side = (resting.side == 'B') ? 'S' : 'B';
            ++gt_labeled;
        }

        // 'E' has no price on the wire — it fills at the resting order's own
        // book price. 'C' fills away from that price; use the wire value.
        uint32_t price = m.has_price ? m.price
                                      : (have_resting ? resting.price : 0);

        BookSnapshot snap = book.top_of_book(m.timestamp_ns);

        bool printable_row = (m.printable != 'N');
        if (!printable_row) ++non_printable_skipped;

        // Row is still written (with printable=N marked) so downstream code
        // can make its own inclusion decision explicitly, rather than this
        // extractor silently dropping data.
        out << m.timestamp_ns << "," << (m.has_price ? 'C' : 'E') << ","
            << price << "," << m.executed_shares << "," << gt_side << ","
            << resting_side_out << "," << snap.best_bid_price << ","
            << snap.best_ask_price << "," << m.printable << "\n";

        book.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
        ++msg_count;
        ++trade_count;
    };
    cb.on_trade_hidden = [&](const TradeMsg& m) {
        // No book mutation — 'P' trades are non-displayed, never in the book.
        BookSnapshot snap = book.top_of_book(m.timestamp_ns);
        out << m.timestamp_ns << ",P," << m.price << "," << m.shares << ",U,U,"
            << snap.best_bid_price << "," << snap.best_ask_price << ",Y\n";
        ++msg_count;
        ++trade_count;
    };

    auto t0 = std::chrono::steady_clock::now();
    try {
        ItchParser::parse_file(itch_file, cb, {locate});
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "Done.\n"
              << "  Messages processed: " << msg_count << "\n"
              << "  Trades (E+C+P):     " << trade_count << "\n"
              << "  Ground-truth labeled (E/C only): " << gt_labeled << "\n"
              << "  Non-printable 'C' rows (flagged, not dropped): " << non_printable_skipped << "\n"
              << "  Elapsed: " << elapsed_s << "s\n"
              << "Trades written to: " << output_csv << "\n";
    return 0;
}
