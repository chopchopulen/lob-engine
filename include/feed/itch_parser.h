#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "feed/itch_types.h"

// ─────────────────────────────────────────────────────────────────────────────
// ItchParser: reads a Nasdaq ITCH 5.0 binary file and fires callbacks
// for each message type we care about.
//
// HOW THE FILE FORMAT WORKS:
//   The raw file is a stream of messages. Each message is prefixed with
//   a 2-byte length field, then the message bytes. So the file looks like:
//
//     [len=11][msg bytes x11][len=36][msg bytes x36][len=19]...
//
//   We read length, read that many bytes, look at byte[0] for the type,
//   then parse the fields at known byte offsets (from the ITCH spec).
//
// The "callbacks" pattern: instead of returning data, the parser calls
// functions you give it when each message type is found. This is the
// "observer" or "event-driven" pattern — common in low-latency systems
// because it avoids allocating a giant vector of all messages.
// ─────────────────────────────────────────────────────────────────────────────

struct ParserCallbacks {
    std::function<void(const AddOrderMsg&)>     on_add;
    std::function<void(const DeleteOrderMsg&)>  on_delete;
    std::function<void(const ReplaceOrderMsg&)> on_replace;
    std::function<void(const ExecuteOrderMsg&)> on_execute;
};

class ItchParser {
public:
    // Parse the full file, firing callbacks for each relevant message.
    // Returns number of messages processed.
    // filter_stock: if non-empty, only fire callbacks for this ticker (e.g. "AAPL")
    static size_t parse_file(const std::string& path,
                             const ParserCallbacks& cb,
                             const std::string& filter_stock = "");
};
