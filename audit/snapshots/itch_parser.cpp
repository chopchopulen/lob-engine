#include "feed/itch_parser.h"
#include <cstdio>
#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Byte offsets for each message type (from ITCH 5.0 spec, Table 4.2)
// Offset 0 is always the message type byte.
// Offset 1-2 is stock_locate (we skip it).
// Offset 3-4 is tracking_number (we skip it).
// Offset 5-10 is timestamp (6 bytes, 48-bit nanoseconds).
// Then fields specific to each message type follow.
// ─────────────────────────────────────────────────────────────────────────────

size_t ItchParser::parse_file(const std::string& path,
                               const ParserCallbacks& cb,
                               const std::string& filter_stock) {
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
            // Read 8-char stock, null-terminate after stripping trailing spaces
            char stk8[9];
            memcpy(stk8, msg + 24, 8);
            stk8[8] = '\0';
            // Strip trailing spaces to get clean ticker
            for (int i = 7; i >= 0 && stk8[i] == ' '; --i) stk8[i] = '\0';

            if (!filter_stock.empty() && strcmp(stk8, filter_stock.c_str()) != 0)
                continue;

            AddOrderMsg m;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            m.side         = (char)msg[19];
            m.shares       = read_u32(msg + 20);
            strncpy(m.stock, stk8, 4); m.stock[4] = '\0';
            m.price        = read_u32(msg + 32);
            cb.on_add(m);
            ++count;
        }

        // ── 'F' Add Order with MPID Attribution ──────────────────────
        // Same as 'A' but 4 extra bytes for market participant ID at end.
        // We parse it identically — just ignore the MPID.
        else if (msg_type == 'F' && cb.on_add) {
            if (msg_len < 40) { continue; }
            char stk8[9];
            memcpy(stk8, msg + 24, 8);
            stk8[8] = '\0';
            for (int i = 7; i >= 0 && stk8[i] == ' '; --i) stk8[i] = '\0';

            if (!filter_stock.empty() && strcmp(stk8, filter_stock.c_str()) != 0)
                continue;

            AddOrderMsg m;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            m.side         = (char)msg[19];
            m.shares       = read_u32(msg + 20);
            strncpy(m.stock, stk8, 4); m.stock[4] = '\0';
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
            DeleteOrderMsg m;
            m.timestamp_ns = read_u48(msg + 5);
            m.order_ref    = read_u64(msg + 11);
            cb.on_delete(m);
            ++count;
        }

        // ── 'U' Replace Order ────────────────────────────────────────
        //  [0]     = 'U'
        //  [5-10]  = timestamp
        //  [11-18] = original order ref
        //  [19-26] = new order ref
        //  [27-30] = new shares
        //  [31-34] = new price
        else if (msg_type == 'U' && cb.on_replace) {
            if (msg_len < 35) continue;
            ReplaceOrderMsg m;
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
        //  [5-10]  = timestamp
        //  [11-18] = order_ref_num
        //  [19-22] = executed_shares
        else if (msg_type == 'E' && cb.on_execute) {
            if (msg_len < 23) continue;
            ExecuteOrderMsg m;
            m.timestamp_ns    = read_u48(msg + 5);
            m.order_ref       = read_u64(msg + 11);
            m.executed_shares = read_u32(msg + 19);
            cb.on_execute(m);
            ++count;
        }
    }

    fclose(f);
    return count;
}
