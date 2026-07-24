#include "book/order_book.h"
#include "feed/itch_parser.h"
#include <cassert>
#include <iostream>
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
    test_replace_and_cross_ticker_order_ref_collision();
    std::cout << "\nAll 13 tests passed.\n";
    return 0;
}
