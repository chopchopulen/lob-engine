#include "book/order_book.h"
#include <cassert>
#include <iostream>

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
    std::cout << "\nAll 10 tests passed.\n";
    return 0;
}
