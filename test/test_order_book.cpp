#include "book/order_book.h"
#include "feed/itch_parser.h"
#include "features/feature_engine.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

// ── test_basic_top_of_book ────────────────────────────────────────────────────
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
static void test_multiple_orders_same_level() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9900);
    book.add_order(3, 3, 'B', 150, 9900);

    auto snap = book.top_of_book(4);
    assert(snap.best_bid_price  == 9900);
    assert(snap.best_bid_shares == 450);

    book.delete_order(5, 2);
    auto snap2 = book.top_of_book(6);
    assert(snap2.best_bid_price  == 9900);
    assert(snap2.best_bid_shares == 250);
    std::cout << "PASS test_multiple_orders_same_level\n";
}

// ── test_delete_removes_level ─────────────────────────────────────────────────
static void test_delete_removes_level() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.add_order(2, 2, 'B', 200, 9800);
    book.delete_order(3, 1);

    auto snap = book.top_of_book(4);
    assert(snap.best_bid_price == 9800);

    auto levels = book.bid_levels(5);
    assert(levels.size() == 1);
    assert(levels[0].first == 9800);
    std::cout << "PASS test_delete_removes_level\n";
}

// ── test_overflow_and_promotion_bids ─────────────────────────────────────────
// Add 21 bid levels (TOP_LEVELS=20 means the 1st/worst overflows).
// refs 1..21 → prices 9800..9820. After adding all 21:
//   flat: 9801..9820 (top 20), overflow: 9800
// Delete best flat level (9820, ref=21) → 9800 promoted from overflow.
static void test_overflow_and_promotion_bids() {
    OrderBook book("TEST");
    for (int i = 1; i <= 21; ++i)
        book.add_order(i, i, 'B', 100, 9800 + (i - 1));

    auto levels = book.bid_levels(21);
    assert(levels.size() == 21);
    assert(levels[0].first  == 9820);
    assert(levels[20].first == 9800);

    book.delete_order(100, 21);  // delete ref=21 (price=9820)

    auto snap = book.top_of_book(101);
    assert(snap.best_bid_price == 9819);

    auto levels2 = book.bid_levels(20);
    assert(levels2.size() == 20);
    assert(levels2.back().first == 9800);  // promoted
    std::cout << "PASS test_overflow_and_promotion_bids\n";
}

// ── test_overflow_and_promotion_asks ─────────────────────────────────────────
// refs 1..21 → prices 10000..10020.
// flat: 10000..10019 (top 20 lowest), overflow: 10020
// Delete best ask (10000, ref=1) → 10020 promoted from overflow.
static void test_overflow_and_promotion_asks() {
    OrderBook book("TEST");
    for (int i = 1; i <= 21; ++i)
        book.add_order(i, i, 'S', 100, 10000 + (i - 1));

    auto levels = book.ask_levels(21);
    assert(levels.size() == 21);
    assert(levels[0].first  == 10000);
    assert(levels[20].first == 10020);

    book.delete_order(100, 1);  // delete ref=1 (price=10000)

    auto snap = book.top_of_book(101);
    assert(snap.best_ask_price == 10001);

    auto levels2 = book.ask_levels(20);
    assert(levels2.size() == 20);
    assert(levels2.back().first == 10020);  // promoted
    std::cout << "PASS test_overflow_and_promotion_asks\n";
}

// ── test_execute_order ────────────────────────────────────────────────────────
static void test_execute_order() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'S', 100, 10000);

    book.execute_order(2, 1, 50);
    auto snap = book.top_of_book(3);
    assert(snap.best_ask_price  == 10000);
    assert(snap.best_ask_shares == 50);

    book.execute_order(4, 1, 50);
    auto snap2 = book.top_of_book(5);
    assert(snap2.best_ask_price == 0);
    auto empty_levels = book.ask_levels(1);
    assert(empty_levels.empty());  // level is fully gone
    std::cout << "PASS test_execute_order\n";
}

// ── test_replace_order ────────────────────────────────────────────────────────
static void test_replace_order() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'B', 100, 9900);
    book.replace_order(2, 1, 2, 200, 9950);

    auto snap = book.top_of_book(3);
    assert(snap.best_bid_price  == 9950);
    assert(snap.best_bid_shares == 200);

    auto levels = book.bid_levels(5);
    assert(levels.size() == 1);
    assert(levels[0].first == 9950);
    // Confirm old ref is gone: deleting it should be a no-op and not change the book.
    book.delete_order(4, 1);
    auto snap2 = book.top_of_book(5);
    assert(snap2.best_bid_price  == 9950);
    assert(snap2.best_bid_shares == 200);  // unchanged — ref=1 was already removed
    std::cout << "PASS test_replace_order\n";
}

// ── test_cancel_order ─────────────────────────────────────────────────────────
static void test_cancel_order() {
    OrderBook book("TEST");
    book.add_order(1, 1, 'S', 100, 10000);

    // Partial cancel: reduces shares but keeps the order/level alive.
    book.cancel_order(2, 1, 30);
    auto snap = book.top_of_book(3);
    assert(snap.best_ask_price  == 10000);
    assert(snap.best_ask_shares == 70);
    assert(book.num_orders() == 1);

    // Full cancel (canceled_shares >= remaining): removes the order/level.
    book.cancel_order(4, 1, 999);
    auto snap2 = book.top_of_book(5);
    assert(snap2.best_ask_price == 0);
    auto empty_levels = book.ask_levels(1);
    assert(empty_levels.empty());
    assert(book.num_orders() == 0);
    std::cout << "PASS test_cancel_order\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH parser tests — stock_locate-based filtering (replaces the old
// literal-ticker-string / order_ref-lookup-miss filtering; see itch_parser.cpp).
// These build tiny synthetic ITCH binary files on disk (the parser only reads
// from a path) using helpers that match the real wire layout exactly.
// ─────────────────────────────────────────────────────────────────────────────

namespace itch_test {

static void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((v >> 8) & 0xFF); b.push_back(v & 0xFF);
}
static void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((v >> 24) & 0xFF); b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 8) & 0xFF);  b.push_back(v & 0xFF);
}
static void put_u48(std::vector<uint8_t>& b, uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8) b.push_back((v >> shift) & 0xFF);
}
static void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) b.push_back((v >> shift) & 0xFF);
}
static void put_stock8(std::vector<uint8_t>& b, const std::string& ticker) {
    std::string padded = ticker;
    padded.resize(8, ' ');
    for (char c : padded) b.push_back((uint8_t)c);
}
static void write_msg(std::vector<uint8_t>& file, const std::vector<uint8_t>& body) {
    put_u16(file, (uint16_t)body.size());
    file.insert(file.end(), body.begin(), body.end());
}

static std::vector<uint8_t> stock_directory(uint16_t locate, const std::string& ticker) {
    std::vector<uint8_t> b;
    b.push_back('R');
    put_u16(b, locate);
    put_u16(b, 0);              // tracking_number
    put_u48(b, 0);               // timestamp
    put_stock8(b, ticker);
    // Pad remaining directory fields (market category .. inverse indicator)
    // to reach the real 39-byte total; values don't matter, not decoded.
    b.resize(38, 0);
    return b;
}
static std::vector<uint8_t> add_order(uint16_t locate, uint64_t ts, uint64_t ref,
                                       char side, uint32_t shares,
                                       const std::string& ticker, uint32_t price) {
    std::vector<uint8_t> b;
    b.push_back('A');
    put_u16(b, locate);
    put_u16(b, 0);
    put_u48(b, ts);
    put_u64(b, ref);
    b.push_back((uint8_t)side);
    put_u32(b, shares);
    put_stock8(b, ticker);
    put_u32(b, price);
    return b;
}
static std::vector<uint8_t> delete_order(uint16_t locate, uint64_t ts, uint64_t ref) {
    std::vector<uint8_t> b;
    b.push_back('D');
    put_u16(b, locate);
    put_u16(b, 0);
    put_u48(b, ts);
    put_u64(b, ref);
    return b;
}
static std::vector<uint8_t> replace_order(uint16_t locate, uint64_t ts,
                                           uint64_t old_ref, uint64_t new_ref,
                                           uint32_t new_shares, uint32_t new_price) {
    std::vector<uint8_t> b;
    b.push_back('U');
    put_u16(b, locate);
    put_u16(b, 0);
    put_u48(b, ts);
    put_u64(b, old_ref);
    put_u64(b, new_ref);
    put_u32(b, new_shares);
    put_u32(b, new_price);
    return b;
}

static std::vector<uint8_t> system_event(char event_code) {
    std::vector<uint8_t> b;
    b.push_back('S');
    put_u16(b, 0);   // stock_locate (unused for this message type, always 0)
    put_u16(b, 0);   // tracking_number
    put_u48(b, 0);   // timestamp
    b.push_back((uint8_t)event_code);
    return b;   // 12 bytes total, matches the real wire layout exactly
}

static void write_file(const std::string& path, const std::vector<std::vector<uint8_t>>& msgs) {
    std::vector<uint8_t> file;
    for (const auto& m : msgs) write_msg(file, m);
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
}

} // namespace itch_test

// ── test_stock_locate_resolution ─────────────────────────────────────────────
static void test_stock_locate_resolution() {
    using namespace itch_test;
    const std::string path = "/tmp/lob_test_locate_resolution.bin";
    write_file(path, {
        stock_directory(1, "AAPL"),
        stock_directory(2, "MSFT"),
        add_order(1, 100, 1, 'B', 100, "AAPL", 10000),
    });

    auto locates = ItchParser::parse_stock_directory(path);
    assert(locates.size() == 2);
    assert(locates.at(1) == "AAPL");
    assert(locates.at(2) == "MSFT");

    remove(path.c_str());
    std::cout << "PASS test_stock_locate_resolution\n";
}

// ── test_locate_filter_excludes_other_symbol ─────────────────────────────────
static void test_locate_filter_excludes_other_symbol() {
    using namespace itch_test;
    const std::string path = "/tmp/lob_test_locate_exclude.bin";
    write_file(path, {
        stock_directory(1, "AAPL"),
        stock_directory(2, "MSFT"),
        add_order(1, 100, 10, 'B', 100, "AAPL", 10000),
        add_order(2, 101, 20, 'S', 200, "MSFT", 30000),
        add_order(1, 102, 11, 'B', 150, "AAPL", 10100),
    });

    int fired = 0;
    ParserCallbacks cb;
    cb.on_add = [&](const AddOrderMsg& m) {
        assert(m.stock_locate == 1);   // MSFT's message must never reach here
        ++fired;
    };
    size_t count = ItchParser::parse_file(path, cb, {1});
    assert(fired == 2);   // only the two AAPL adds
    assert(count == 2);

    remove(path.c_str());
    std::cout << "PASS test_locate_filter_excludes_other_symbol\n";
}

// ── test_stock_directory_early_exit ──────────────────────────────────────────
// parse_stock_directory() stops scanning at the first System Event other than
// 'O' (see itch_parser.cpp) instead of reading the whole file. Confirms it
// actually stops -- not just that it doesn't crash -- by placing a Stock
// Directory message AFTER that marker and checking it is NOT picked up.
static void test_stock_directory_early_exit() {
    using namespace itch_test;
    const std::string path = "/tmp/lob_test_directory_early_exit.bin";
    write_file(path, {
        system_event('O'),          // start of messages
        stock_directory(1, "AAAA"),
        stock_directory(2, "BBBB"),
        system_event('S'),          // start of system hours -- pre-session ends here
        stock_directory(3, "CCCC"), // arrives after the marker -- must be skipped
    });

    auto locates = ItchParser::parse_stock_directory(path);
    assert(locates.size() == 2);
    assert(locates.at(1) == "AAAA");
    assert(locates.at(2) == "BBBB");
    assert(locates.find(3) == locates.end());

    remove(path.c_str());
    std::cout << "PASS test_stock_directory_early_exit\n";
}

// ── test_late_stock_directory_hard_errors ────────────────────────────────────
// If a 'R' message ever appears after parse_stock_directory()'s early-exit
// point (a spec violation the early-exit assumes cannot happen), parse_file()
// must fail loudly rather than silently proceed with a possibly-incomplete
// locate map that could mis-book a later message.
static void test_late_stock_directory_hard_errors() {
    using namespace itch_test;
    const std::string path = "/tmp/lob_test_late_r.bin";
    write_file(path, {
        system_event('O'),
        stock_directory(1, "AAAA"),
        system_event('S'),
        stock_directory(2, "BBBB"),   // late 'R' -- violates the ordering guarantee
        add_order(1, 100, 1, 'B', 100, "AAAA", 10000),
    });

    ParserCallbacks cb;
    cb.on_add = [](const AddOrderMsg&) {};
    bool threw = false;
    try {
        ItchParser::parse_file(path, cb, {1});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    remove(path.c_str());
    std::cout << "PASS test_late_stock_directory_hard_errors\n";
}

// ── test_replace_and_cross_ticker_order_ref_collision ────────────────────────
// The actual bug this replaces: 'D'/'U'/'E'/'C'/'X' used to be applied to
// whatever single book was open, filtered only by whether order_ref happened
// to be present. Two tickers sharing the same order_ref value (deliberately
// constructed here) would have let ticker2's Delete corrupt ticker1's book.
// stock_locate filtering must prevent this regardless of order_ref collisions.
static void test_replace_and_cross_ticker_order_ref_collision() {
    using namespace itch_test;
    const std::string path = "/tmp/lob_test_locate_collision.bin";
    const uint64_t shared_ref = 42;   // same order_ref used by both tickers
    write_file(path, {
        stock_directory(1, "AAPL"),
        stock_directory(2, "MSFT"),
        add_order(1, 100, shared_ref, 'B', 100, "AAPL", 10000),
        add_order(2, 101, shared_ref, 'S', 999, "MSFT", 50000),
        // Replace AAPL's order: mints a new order_ref (200), same instrument.
        replace_order(1, 102, shared_ref, 200, 150, 10050),
        // Delete MSFT's order (same order_ref value as AAPL's original ref)
        // — must NOT be delivered when filtering for AAPL only.
        delete_order(2, 103, shared_ref),
    });

    int adds = 0, replaces = 0, deletes = 0;
    ParserCallbacks cb;
    cb.on_add     = [&](const AddOrderMsg&)     { ++adds; };
    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        assert(m.stock_locate == 1);
        assert(m.old_order_ref == shared_ref);
        assert(m.new_order_ref == 200);
        assert(m.new_shares == 150);
        ++replaces;
    };
    cb.on_delete = [&](const DeleteOrderMsg&) { ++deletes; };

    ItchParser::parse_file(path, cb, {1});   // AAPL only
    assert(adds == 1);       // only AAPL's add
    assert(replaces == 1);   // AAPL's replace
    assert(deletes == 0);    // MSFT's delete correctly excluded despite ref collision

    remove(path.c_str());
    std::cout << "PASS test_replace_and_cross_ticker_order_ref_collision\n";
}

// ── test_golden_fixture_pipeline ──────────────────────────────────────────────
// Byte-for-byte regression test on the full ITCH-file -> CSV pipeline, run
// against a real slice of Nasdaq BX ITCH data (test/fixtures/itch_sample_slice.bin,
// 2019-07-30, AAPL + MSFT, ~200KB, 2,935 real messages). Mirrors main.cpp's
// single-ticker pipeline exactly (resolve locate via Stock Directory,
// OrderBook + FeatureEngine + ItchParser::parse_file, write_csv).
//
// Verified 2026-07-24 to catch real parser regressions: deliberately
// misaligning the 'A' message's price read by 4 bytes (msg+32 -> msg+28)
// makes this test fail with a byte-level diff, as expected for a live
// wire-format bug. NOTE: it does NOT catch a literal replay of the original
// item-4 "4-byte stock field" bug (AddOrderMsg.stock truncated to 4 chars) —
// that bug only ever affected the AddOrderMsg.stock struct field, which
// nothing in today's pipeline reads (ticker filtering uses stock_locate, not
// the stock string, since the Task 2 refactor). Confirmed empirically:
// reverting that exact historical diff and rerunning this test produces
// byte-identical output. Kept as an honest record rather than silently
// claiming coverage this test doesn't have — see results/OFI_STUDY.md
// "Blocker 1" for the related retraction this finding triggered.
static void run_fixture_pipeline(const std::string& ticker, const std::string& expected_path) {
    const std::string fixture_bin = "../test/fixtures/itch_sample_slice.bin";
    const std::string actual_path = "/tmp/lob_test_fixture_actual_" + ticker + ".csv";

    auto locates = ItchParser::parse_stock_directory(fixture_bin);
    uint16_t locate = 0;
    bool found = false;
    for (const auto& [loc, sym] : locates) {
        if (sym == ticker) { locate = loc; found = true; break; }
    }
    assert(found && "ticker not found in fixture's Stock Directory");

    OrderBook     book(ticker);
    FeatureEngine features;

    ParserCallbacks cb;
    cb.on_add = [&](const AddOrderMsg& m) {
        book.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
        features.on_book_update(book, m.timestamp_ns);
    };
    cb.on_delete = [&](const DeleteOrderMsg& m) {
        book.delete_order(m.timestamp_ns, m.order_ref);
        features.on_book_update(book, m.timestamp_ns);
    };
    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        book.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                           m.new_shares, m.new_price);
        features.on_book_update(book, m.timestamp_ns);
    };
    cb.on_execute = [&](const ExecuteOrderMsg& m) {
        features.on_trade('B', m.executed_shares, m.timestamp_ns);
        book.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
        features.on_book_update(book, m.timestamp_ns);
    };
    cb.on_cancel = [&](const CancelOrderMsg& m) {
        book.cancel_order(m.timestamp_ns, m.order_ref, m.canceled_shares);
        features.on_book_update(book, m.timestamp_ns);
    };

    ItchParser::parse_file(fixture_bin, cb, {locate});
    features.write_csv(actual_path);

    std::ifstream actual_f(actual_path, std::ios::binary);
    std::ifstream expected_f(expected_path, std::ios::binary);
    assert(actual_f.good() && "failed to open actual output");
    assert(expected_f.good() && "failed to open expected fixture — did you run from build_audit/?");

    std::stringstream actual_ss, expected_ss;
    actual_ss << actual_f.rdbuf();
    expected_ss << expected_f.rdbuf();

    if (actual_ss.str() != expected_ss.str()) {
        std::cerr << "MISMATCH for " << ticker << ": actual output does not match "
                  << expected_path << " byte-for-byte.\n"
                  << "  actual:   " << actual_path << " (" << actual_ss.str().size() << " bytes)\n"
                  << "  expected: " << expected_path << " (" << expected_ss.str().size() << " bytes)\n";
        assert(false && "golden fixture mismatch — see stderr above");
    }

    remove(actual_path.c_str());
    std::cout << "PASS test_golden_fixture_pipeline[" << ticker << "]\n";
}

static void test_golden_fixture_pipeline() {
    run_fixture_pipeline("AAPL", "../test/fixtures/expected_features_AAPL.csv");
    run_fixture_pipeline("MSFT", "../test/fixtures/expected_features_MSFT.csv");
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
    test_cancel_order();
    test_stock_locate_resolution();
    test_locate_filter_excludes_other_symbol();
    test_stock_directory_early_exit();
    test_late_stock_directory_hard_errors();
    test_replace_and_cross_ticker_order_ref_collision();
    test_golden_fixture_pipeline();
    std::cout << "\nAll 16 tests passed.\n";
    return 0;
}
