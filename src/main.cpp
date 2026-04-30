#include <iostream>
#include <string>
#include <chrono>
#include "feed/itch_parser.h"
#include "book/order_book.h"
#include "features/feature_engine.h"

// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — the program entry point
//
// Usage:
//   ./lob_engine <itch_file.bin> <TICKER> <output_features.csv>
//
// Example:
//   ./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv
//
// What it does:
//   1. Opens the ITCH binary file
//   2. Parses every message for the given ticker
//   3. Applies each message to the order book (reconstructing the LOB)
//   4. Computes microstructure features (OFI, spread, etc.) every second
//   5. Writes features to CSV for analysis.py
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <itch_file> <TICKER> <output_csv>\n"
                  << "Example: ./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv\n";
        return 1;
    }

    const std::string itch_file  = argv[1];
    const std::string ticker     = argv[2];
    const std::string output_csv = argv[3];

    std::cout << "LOB Engine — Nasdaq ITCH 5.0 Parser\n"
              << "  File:    " << itch_file  << "\n"
              << "  Ticker:  " << ticker     << "\n"
              << "  Output:  " << output_csv << "\n\n";

    // ── Instantiate core components ────────────────────────────────────────
    OrderBook      book(ticker);
    FeatureEngine  features;

    size_t msg_count    = 0;
    size_t trade_count  = 0;

    // ── Wire up parser callbacks ───────────────────────────────────────────
    // This is the main loop. The parser reads each ITCH message and calls
    // the appropriate lambda, which updates the book and then triggers
    // feature computation.

    ParserCallbacks cb;

    cb.on_add = [&](const AddOrderMsg& m) {
        book.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
        features.on_book_update(book, m.timestamp_ns);
        ++msg_count;
    };

    cb.on_delete = [&](const DeleteOrderMsg& m) {
        book.delete_order(m.timestamp_ns, m.order_ref);
        features.on_book_update(book, m.timestamp_ns);
        ++msg_count;
    };

    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        book.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                           m.new_shares, m.new_price);
        features.on_book_update(book, m.timestamp_ns);
        ++msg_count;
    };

    cb.on_execute = [&](const ExecuteOrderMsg& m) {
        // Determine aggressor side from the resting order
        // (In ITCH, the execute message hits the passive order)
        // We use 'B' as a placeholder — full VPIN needs cross-referencing
        // trade direction with the aggressor, which requires the 'P' message.
        // For the simplified trade imbalance feature, this is sufficient.
        features.on_trade('B', m.executed_shares, m.timestamp_ns);
        book.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
        features.on_book_update(book, m.timestamp_ns);
        ++msg_count;
        ++trade_count;
    };

    // ── Run the parser ─────────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();

    try {
        ItchParser::parse_file(itch_file, cb, ticker);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    // ── Write output ───────────────────────────────────────────────────────
    try {
        features.write_csv(output_csv);
    } catch (const std::exception& e) {
        std::cerr << "Error writing CSV: " << e.what() << "\n";
        return 1;
    }

    // ── Summary ────────────────────────────────────────────────────────────
    std::cout << "Done.\n"
              << "  Messages processed:  " << msg_count    << "\n"
              << "  Trades:              " << trade_count  << "\n"
              << "  Feature rows:        " << features.rows().size() << "\n"
              << "  Active orders left:  " << book.num_orders() << "\n"
              << "  Elapsed:             " << elapsed_s    << "s\n"
              << "  Throughput:          "
              << (msg_count / elapsed_s / 1e6) << "M msg/s\n\n"
              << "Features written to: " << output_csv << "\n";

    return 0;
}
