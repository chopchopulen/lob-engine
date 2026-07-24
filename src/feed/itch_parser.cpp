#include "feed/itch_parser.h"
#include <cstdio>
#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Byte offsets for each message type (from ITCH 5.0 spec, Table 4.2)
// Offset 0 is always the message type byte.
// Offset 1-2 is stock_locate — every message type below reads and filters on
// this field. It is the exchange's per-instrument key; ticker symbol strings
// only appear on 'A'/'F'/'R' messages, never on 'D'/'U'/'E'/'C'/'X', so
// stock_locate is the only field common to every message type that can be
// used to route/filter by instrument.
// Offset 3-4 is tracking_number (we skip it — not needed downstream).
// Offset 5-10 is timestamp (6 bytes, 48-bit nanoseconds).
// Then fields specific to each message type follow.
// ─────────────────────────────────────────────────────────────────────────────

static inline bool passes_filter(uint16_t locate,
                                  const std::unordered_set<uint16_t>& filter_locates) {
    return filter_locates.empty() || filter_locates.count(locate) != 0;
}

std::unordered_map<uint16_t, std::string>
ItchParser::parse_stock_directory(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open file: " + path);

    std::unordered_map<uint16_t, std::string> locates;

    uint8_t len_buf[2];
    uint8_t msg[1024];

    while (true) {
        if (fread(len_buf, 1, 2, f) != 2) break;
        uint16_t msg_len = read_u16(len_buf);
        if (msg_len == 0 || msg_len > sizeof(msg)) {
            fseek(f, msg_len, SEEK_CUR);
            continue;
        }
        if (fread(msg, 1, msg_len, f) != msg_len) break;

        // ── 'R' Stock Directory ──────────────────────────────────────
        //  [0]     = 'R'
        //  [1-2]   = stock_locate
        //  [3-4]   = tracking_number
        //  [5-10]  = timestamp
        //  [11-18] = stock (8 bytes, right-padded with spaces)
        //  [19..]  = market category / classification / LULD tier / etc.
        //            (not decoded — not needed downstream)
        if ((char)msg[0] == 'R' && msg_len >= 19) {
            uint16_t locate = read_u16(msg + 1);
            char stk8[9];
            memcpy(stk8, msg + 11, 8);
            stk8[8] = '\0';
            for (int i = 7; i >= 0 && stk8[i] == ' '; --i) stk8[i] = '\0';
            locates[locate] = std::string(stk8);
        }
    }

    fclose(f);
    return locates;
}

size_t ItchParser::parse_file(const std::string& path,
                               const ParserCallbacks& cb,
                               const std::unordered_set<uint16_t>& filter_locates) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open file: " + path);

    uint8_t len_buf[2];
    uint8_t msg[1024];
    size_t  count = 0;

    while (true) {
        // Read 2-byte message length prefix
        if (fread(len_buf, 1, 2, f) != 2) break;
        uint16_t msg_len = read_u16(len_buf);
        if (msg_len == 0 || msg_len > sizeof(msg)) {
            // Skip malformed message
            fseek(f, msg_len, SEEK_CUR);
            continue;
        }

        // Read the message body
        if (fread(msg, 1, msg_len, f) != msg_len) break;

        char msg_type = (char)msg[0];

        // ── 'A' Add Order (no MPID) ──────────────────────────────────
        // Nasdaq ITCH 5.0 spec, Section 4.4.1 — exactly 36 bytes:
        //  [0]     = message type 'A'
        //  [1-2]   = stock_locate      (2 bytes)
        //  [3-4]   = tracking_number   (2 bytes)
        //  [5-10]  = timestamp         (6 bytes, 48-bit nanoseconds)
        //  [11-18] = order_ref_num     (8 bytes, 64-bit)
        //  [19]    = buy_sell_indicator(1 byte, 'B' or 'S')
        //  [20-23] = shares            (4 bytes, 32-bit)
        //  [24-31] = stock             (8 bytes! right-padded with spaces)
        //  [32-35] = price             (4 bytes, 32-bit, 1/10000 dollars)
        // NOTE: stock is 8 chars in ITCH 5.0, not 4. This was the bug.
        if (msg_type == 'A' && cb.on_add) {
            if (msg_len < 36) { continue; }
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            char stk8[9];
            memcpy(stk8, msg + 24, 8);
            stk8[8] = '\0';
            for (int i = 7; i >= 0 && stk8[i] == ' '; --i) stk8[i] = '\0';

            AddOrderMsg m;
            m.stock_locate = locate;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            m.side         = (char)msg[19];
            m.shares       = read_u32(msg + 20);
            strncpy(m.stock, stk8, 8); m.stock[8] = '\0';
            m.price        = read_u32(msg + 32);
            cb.on_add(m);
            ++count;
        }

        // ── 'F' Add Order with MPID Attribution ──────────────────────
        // Same as 'A' but 4 extra bytes for market participant ID at end.
        // We parse it identically — just ignore the MPID.
        else if (msg_type == 'F' && cb.on_add) {
            if (msg_len < 40) { continue; }
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            char stk8[9];
            memcpy(stk8, msg + 24, 8);
            stk8[8] = '\0';
            for (int i = 7; i >= 0 && stk8[i] == ' '; --i) stk8[i] = '\0';

            AddOrderMsg m;
            m.stock_locate = locate;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            m.side         = (char)msg[19];
            m.shares       = read_u32(msg + 20);
            strncpy(m.stock, stk8, 8); m.stock[8] = '\0';
            m.price        = read_u32(msg + 32);
            cb.on_add(m);
            ++count;
        }

        // ── 'D' Delete Order ─────────────────────────────────────────
        //  [0]     = 'D'
        //  [1-2]   = stock_locate
        //  [3-4]   = tracking_number
        //  [5-10]  = timestamp (48-bit ns)
        //  [11-18] = order_ref_num (64-bit)
        else if (msg_type == 'D' && cb.on_delete) {
            if (msg_len < 19) continue;
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            DeleteOrderMsg m;
            m.stock_locate = locate;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            cb.on_delete(m);
            ++count;
        }

        // ── 'U' Replace Order ────────────────────────────────────────
        //  [0]     = 'U'
        //  [1-2]   = stock_locate (same instrument as the order being replaced)
        //  [5-10]  = timestamp
        //  [11-18] = original order ref
        //  [19-26] = new order ref
        //  [27-30] = new shares
        //  [31-34] = new price
        else if (msg_type == 'U' && cb.on_replace) {
            if (msg_len < 35) continue;
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            ReplaceOrderMsg m;
            m.stock_locate  = locate;
            m.timestamp_ns  = read_u48(msg + 5);
            m.old_order_ref = read_u64(msg + 11);
            m.new_order_ref = read_u64(msg + 19);
            m.new_shares    = read_u32(msg + 27);
            m.new_price     = read_u32(msg + 31);
            cb.on_replace(m);
            ++count;
        }

        // ── 'E' Order Executed ───────────────────────────────────────
        //  [0]     = 'E'
        //  [1-2]   = stock_locate
        //  [5-10]  = timestamp
        //  [11-18] = order_ref_num
        //  [19-22] = executed_shares
        else if (msg_type == 'E' && cb.on_execute) {
            if (msg_len < 23) continue;
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            ExecuteOrderMsg m;
            m.stock_locate    = locate;
            m.timestamp_ns    = read_u48(msg + 5);
            m.order_ref       = read_u64(msg + 11);
            m.executed_shares = read_u32(msg + 19);
            cb.on_execute(m);
            ++count;
        }

        // ── 'C' Order Executed With Price ─────────────────────────────
        //  [0]     = 'C'
        //  [1-2]   = stock_locate
        //  [5-10]  = timestamp
        //  [11-18] = order_ref_num
        //  [19-22] = executed_shares
        //  [23-30] = match_number    (ignored — not needed downstream)
        //  [31]    = printable       (ignored)
        //  [32-35] = execution_price (ignored — book only tracks resting
        //            price; this message type is for executions away from
        //            the order's display price, but the share reduction +
        //            trade count is what matters here)
        // Functionally an execution (like 'E'): reduces/executes an order's
        // shares. Reuses ExecuteOrderMsg + on_execute — OrderBook::execute_order
        // only needs order_ref + executed_shares, and that path is already
        // covered by test_execute_order in test_order_book.cpp.
        else if (msg_type == 'C' && cb.on_execute) {
            if (msg_len < 36) continue;
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            ExecuteOrderMsg m;
            m.stock_locate    = locate;
            m.timestamp_ns    = read_u48(msg + 5);
            m.order_ref       = read_u64(msg + 11);
            m.executed_shares = read_u32(msg + 19);
            cb.on_execute(m);
            ++count;
        }

        // ── 'X' Order Cancel (partial) ───────────────────────────────
        //  [0]     = 'X'
        //  [1-2]   = stock_locate
        //  [5-10]  = timestamp
        //  [11-18] = order_ref_num
        //  [19-22] = canceled_shares
        else if (msg_type == 'X' && cb.on_cancel) {
            if (msg_len < 23) continue;
            uint16_t locate = read_u16(msg + 1);
            if (!passes_filter(locate, filter_locates)) continue;

            CancelOrderMsg m;
            m.stock_locate     = locate;
            m.timestamp_ns     = read_u48(msg + 5);
            m.order_ref        = read_u64(msg + 11);
            m.canceled_shares  = read_u32(msg + 19);
            cb.on_cancel(m);
            ++count;
        }
    }

    fclose(f);
    return count;
}
