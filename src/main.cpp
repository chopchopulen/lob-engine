#include <iostream>
#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include "feed/itch_parser.h"
#include "book/order_book.h"
#include "features/feature_engine.h"

// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — the program entry point
//
// Usage (single-ticker):
//   ./lob_engine <itch_file.bin> <TICKER> <output_features.csv>
//
// Usage (dual-ticker cross-asset):
//   ./lob_engine <itch_file.bin> <TICKER1> <TICKER2> <combined.csv>
//
// Examples:
//   ./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv
//   ./lob_engine data/01302020.NASDAQ_ITCH50 AAPL SPY data/cross_AAPL_SPY.csv
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Usage ──────────────────────────────────────────────────────────────
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <itch_file> <TICKER> <output.csv>\n"
                  << "  " << argv[0] << " <itch_file> <TICKER1> <TICKER2> <combined.csv>\n";
        return 1;
    }

    const std::string itch_file = argv[1];

    // ── Single-ticker mode ─────────────────────────────────────────────────
    if (argc == 4) {
        const std::string ticker     = argv[2];
        const std::string output_csv = argv[3];

        // Timer starts here, BEFORE stock-directory resolution: parse_stock_directory()
        // does its own full sequential scan of the file (looking only for 'R'
        // messages) and previously ran entirely untimed, silently excluded from
        // "Elapsed" below — that undercounted real per-run cost by roughly 2x
        // (see bench/BASELINE.md, "Real end-to-end panel-regeneration measurement").
        // One honest timer now covers the whole run: locate resolution + parse +
        // reconstruct + CSV write.
        auto t0 = std::chrono::steady_clock::now();

        // Resolve ticker -> stock_locate via the file's Stock Directory ('R')
        // messages. Filtering is then done on stock_locate for every message
        // type (see ItchParser::parse_file) — not on order_ref lookup misses.
        auto locate_map = ItchParser::parse_stock_directory(itch_file);
        uint16_t locate = 0;
        bool found = false;
        for (const auto& [loc, sym] : locate_map) {
            if (sym == ticker) { locate = loc; found = true; break; }
        }
        if (!found) {
            std::cerr << "Error: ticker \"" << ticker << "\" not found in this file's "
                      << "Stock Directory ('R') messages. " << locate_map.size()
                      << " symbols were found in the directory.\n";
            return 1;
        }

        std::cout << "LOB Engine — Nasdaq ITCH 5.0 Parser\n"
                  << "  File:    " << itch_file  << "\n"
                  << "  Ticker:  " << ticker << " (stock_locate=" << locate << ")\n"
                  << "  Output:  " << output_csv << "\n\n";

        OrderBook      book(ticker);
        FeatureEngine  features;
        size_t msg_count = 0, trade_count = 0;

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
            features.on_trade('B', m.executed_shares, m.timestamp_ns);
            book.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
            ++trade_count;
        };
        cb.on_cancel = [&](const CancelOrderMsg& m) {
            book.cancel_order(m.timestamp_ns, m.order_ref, m.canceled_shares);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
        };

        try {
            ItchParser::parse_file(itch_file, cb, {locate});
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }

        try {
            features.write_csv(output_csv);
        } catch (const std::exception& e) {
            std::cerr << "Error writing CSV: " << e.what() << "\n";
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

        // "Messages matched" is the count matched to this ticker's
        // stock_locate, not the file's total message count -- the main parse
        // pass always scans the whole file regardless of how few messages
        // match, so "matched messages / elapsed_s" is NOT a meaningful
        // throughput figure (it conflates a filtered numerator with a
        // full-file-scan denominator). Reporting file size / elapsed_s
        // (MB/s) instead, which is unambiguous regardless of filter
        // selectivity. See bench/BASELINE.md "Real end-to-end measurement"
        // for the properly-paired total-messages-scanned/time figure.
        double file_mb = std::filesystem::file_size(itch_file) / 1e6;
        std::cout << "Done.\n"
                  << "  Messages matched (this ticker): " << msg_count   << "\n"
                  << "  Trades:              " << trade_count << "\n"
                  << "  Feature rows:        " << features.rows().size() << "\n"
                  << "  Active orders left:  " << book.num_orders() << "\n"
                  << "  Elapsed (full run: locate resolution + parse + reconstruct + write): "
                  << elapsed_s << "s\n"
                  << "  File size:           " << file_mb << " MB\n"
                  << "  Effective I/O rate:  " << (file_mb / elapsed_s) << " MB/s "
                  << "(main parse scans the whole file once; locate resolution early-exits, see ItchParser::parse_stock_directory)\n\n"
                  << "Features written to: " << output_csv << "\n";
        return 0;
    }

    // ── Dual-ticker cross-asset mode ──────────────────────────────────────
    const std::string ticker1    = argv[2];
    const std::string ticker2    = argv[3];
    const std::string output_csv = argv[4];

    // Timer starts before stock-directory resolution -- see the single-ticker
    // mode comment above for why (that scan was previously untimed, undercounting
    // real per-run cost by roughly 2x).
    auto t0 = std::chrono::steady_clock::now();

    // Resolve both tickers -> stock_locate up front. Every message (including
    // D/U/E/C/X) carries its own stock_locate, so routing below needs no
    // order_ref bookkeeping — the old order_route map (an implicit, unverified
    // "filter by hoping order_ref never collides across tickers" mechanism)
    // is gone.
    auto locate_map = ItchParser::parse_stock_directory(itch_file);
    uint16_t locate1 = 0, locate2 = 0;
    bool found1 = false, found2 = false;
    for (const auto& [loc, sym] : locate_map) {
        if (sym == ticker1) { locate1 = loc; found1 = true; }
        if (sym == ticker2) { locate2 = loc; found2 = true; }
    }
    if (!found1 || !found2) {
        std::cerr << "Error: " << (!found1 ? ticker1 : ticker2)
                  << " not found in this file's Stock Directory ('R') messages. "
                  << locate_map.size() << " symbols were found in the directory.\n";
        return 1;
    }

    std::cout << "LOB Engine — Cross-Asset Mode\n"
              << "  File:    " << itch_file  << "\n"
              << "  Ticker1: " << ticker1 << " (stock_locate=" << locate1 << ")\n"
              << "  Ticker2: " << ticker2 << " (stock_locate=" << locate2 << ")\n"
              << "  Output:  " << output_csv << "\n\n";

    OrderBook     book1(ticker1), book2(ticker2);
    FeatureEngine feat1, feat2;
    size_t cnt1 = 0, cnt2 = 0, trade_cnt1 = 0, trade_cnt2 = 0;

    ParserCallbacks cb;

    cb.on_add = [&](const AddOrderMsg& m) {
        if (m.stock_locate == locate1) {
            book1.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_delete = [&](const DeleteOrderMsg& m) {
        if (m.stock_locate == locate1) {
            book1.delete_order(m.timestamp_ns, m.order_ref);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.delete_order(m.timestamp_ns, m.order_ref);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        // stock_locate on a replace refers to the same instrument as the
        // order being replaced — ITCH 5.0 never changes instrument on 'U'.
        if (m.stock_locate == locate1) {
            book1.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                                m.new_shares, m.new_price);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                                m.new_shares, m.new_price);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_execute = [&](const ExecuteOrderMsg& m) {
        if (m.stock_locate == locate1) {
            feat1.on_trade('B', m.executed_shares, m.timestamp_ns);
            book1.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1; ++trade_cnt1;
        } else {
            feat2.on_trade('B', m.executed_shares, m.timestamp_ns);
            book2.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2; ++trade_cnt2;
        }
    };

    cb.on_cancel = [&](const CancelOrderMsg& m) {
        if (m.stock_locate == locate1) {
            book1.cancel_order(m.timestamp_ns, m.order_ref, m.canceled_shares);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.cancel_order(m.timestamp_ns, m.order_ref, m.canceled_shares);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    // ── Parse ─────────────────────────────────────────────────────────────
    try {
        ItchParser::parse_file(itch_file, cb, {locate1, locate2});
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // ── Guard: both tickers must have produced rows ────────────────────────
    if (feat1.rows().empty()) {
        std::cerr << "Error: " << ticker1 << " produced no feature rows.\n"
                  << "  Verify it appears in the ITCH file.\n";
        return 1;
    }
    if (feat2.rows().empty()) {
        std::cerr << "Error: " << ticker2 << " produced no feature rows.\n"
                  << "  Verify it appears in the ITCH file.\n";
        return 1;
    }

    // ── Merge rows by second-floor bucket ─────────────────────────────────
    std::unordered_map<uint64_t, size_t> by_sec1, by_sec2;
    by_sec1.reserve(feat1.rows().size());
    by_sec2.reserve(feat2.rows().size());

    for (size_t i = 0; i < feat1.rows().size(); ++i)
        by_sec1[feat1.rows()[i].timestamp_ns / 1'000'000'000ULL] = i;
    for (size_t i = 0; i < feat2.rows().size(); ++i)
        by_sec2[feat2.rows()[i].timestamp_ns / 1'000'000'000ULL] = i;

    std::vector<uint64_t> common_secs;
    common_secs.reserve(std::min(feat1.rows().size(), feat2.rows().size()));
    for (const auto& [sec, unused] : by_sec1)
        if (by_sec2.count(sec)) common_secs.push_back(sec);
    std::sort(common_secs.begin(), common_secs.end());

    // Column prefixes: lowercase ticker names
    std::string p1 = ticker1, p2 = ticker2;
    std::transform(p1.begin(), p1.end(), p1.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::transform(p2.begin(), p2.end(), p2.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::ofstream csv_out(output_csv);
    if (!csv_out) {
        std::cerr << "Error: cannot write " << output_csv << "\n";
        return 1;
    }
    csv_out << "ts," << p1 << "_mid," << p1 << "_ofi,"
            << p2 << "_mid," << p2 << "_ofi\n";

    for (uint64_t sec : common_secs) {
        const FeatureRow& r1 = feat1.rows()[by_sec1.at(sec)];
        const FeatureRow& r2 = feat2.rows()[by_sec2.at(sec)];
        csv_out << r1.timestamp_ns << ","
                << r1.mid_price   << "," << r1.ofi << ","
                << r2.mid_price   << "," << r2.ofi << "\n";
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    // See single-ticker mode's comment above: matched-message counts divided
    // by elapsed_s is not a meaningful throughput (filtered numerator, full-
    // file-scan denominator) -- MB/s is unambiguous regardless of filter
    // selectivity.
    double file_mb = std::filesystem::file_size(itch_file) / 1e6;
    std::cout << "Done.\n"
              << "  " << ticker1 << " messages matched: " << cnt1        << "\n"
              << "  " << ticker2 << " messages matched: " << cnt2        << "\n"
              << "  " << ticker1 << " feature rows: " << feat1.rows().size() << "\n"
              << "  " << ticker2 << " feature rows: " << feat2.rows().size() << "\n"
              << "  Combined rows (aligned seconds): " << common_secs.size() << "\n"
              << "  Elapsed (full run: locate resolution + parse + reconstruct + write): "
              << elapsed_s << "s\n"
              << "  File size:          " << file_mb << " MB\n"
              << "  Effective I/O rate: " << (file_mb / elapsed_s) << " MB/s "
              << "(main parse scans the whole file once; locate resolution early-exits, see ItchParser::parse_stock_directory)\n\n"
              << "Combined CSV written to: " << output_csv << "\n";
    return 0;
}
