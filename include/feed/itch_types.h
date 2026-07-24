#pragma once
#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Nasdaq ITCH 5.0 message types
//
// Nasdaq publishes raw order-by-order data in a binary format called ITCH 5.0.
// Every message starts with a 1-byte "type" code, then fixed-size fields.
// All multi-byte integers are big-endian (network byte order).
//
// We care about these message types:
//   'A' = Add Order          (new order arrives at a price level)
//   'D' = Delete Order       (order cancelled)
//   'U' = Replace Order      (cancel + re-add at new price/qty)
//   'E' = Order Executed     (trade happened — order partially/fully filled)
//   'C' = Order Executed With Price (execution away from display price;
//         shares/order_ref semantics identical to 'E' — see ExecuteOrderMsg)
//
// Full spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
// ─────────────────────────────────────────────────────────────────────────────

// Helper: read a big-endian integer from a byte pointer
// The exchange sends data in network byte order (big-endian).
// x86 Macs are little-endian, so we must byte-swap.
inline uint16_t read_u16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | p[1];
}
inline uint32_t read_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
inline uint64_t read_u48(const uint8_t* p) {
    // ITCH uses 6-byte (48-bit) timestamps in nanoseconds
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) |
           (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) << 8)  |  uint64_t(p[5]);
}
inline uint64_t read_u64(const uint8_t* p) {
    return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) |
           (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
           (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) |
           (uint64_t(p[6]) << 8)  |  uint64_t(p[7]);
}

// Read a 4-char stock ticker (e.g. "AAPL") — right-padded with spaces
inline void read_stock(const uint8_t* p, char out[5]) {
    memcpy(out, p, 4);
    out[4] = '\0';
}

// ── Message structs ──────────────────────────────────────────────────────────

// Parsed from 'A' message (36 bytes total in ITCH)
struct AddOrderMsg {
    uint16_t stock_locate;    // exchange-assigned locate code — the real per-message
                              // symbol key (see StockDirectoryMsg); stock[] below is
                              // kept for display/logging, not for filtering
    uint64_t timestamp_ns;   // nanoseconds since midnight
    uint64_t order_ref;      // unique order ID assigned by exchange
    char     side;           // 'B' = buy, 'S' = sell
    uint32_t shares;         // number of shares
    char     stock[9];       // ticker symbol (ITCH 5.0 max is 8 chars + NUL)
    uint32_t price;          // price in units of 1/10000 dollar (e.g. 1500000 = $150.00)
};

// Parsed from 'D' message (19 bytes total)
struct DeleteOrderMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
};

// Parsed from 'U' message (35 bytes total)
// A replace is: cancel old order, add new order at potentially different price/qty.
// stock_locate refers to the SAME stock as the order being replaced — ITCH 5.0
// does not allow a replace to change the underlying instrument.
struct ReplaceOrderMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t old_order_ref;
    uint64_t new_order_ref;
    uint32_t new_shares;
    uint32_t new_price;
};

// Parsed from 'E' message (31 bytes total)
// Partial or full execution — some shares traded at this order's price
struct ExecuteOrderMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t executed_shares;
};

// Parsed from 'X' message (23 bytes total)
// Order Cancel — a PARTIAL cancel (reduces shares), distinct from 'D' Delete
// (which removes the order entirely). No trade occurs.
struct CancelOrderMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t canceled_shares;
};

// Parsed from 'R' Stock Directory message (39 bytes total in ITCH 5.0).
// One per listed instrument, normally sent once at the start of the session
// before any trading messages. This is the authoritative locate->symbol
// mapping; only stock_locate and stock are needed downstream, so the
// remaining ~20 bytes of market-category/classification/LULD-tier fields
// are read past but not decoded (not needed by this engine).
struct StockDirectoryMsg {
    uint16_t stock_locate;
    char     stock[9];   // ticker symbol, right-padded with spaces in the wire format
};
