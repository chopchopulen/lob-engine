#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::function<void(const CancelOrderMsg&)>  on_cancel;
};

class ItchParser {
public:
    // Read the file once, decoding only 'R' Stock Directory messages, and
    // return the locate -> symbol map. Symbols are trimmed of trailing
    // padding spaces (e.g. "AAPL"). Real ITCH feeds send these once per
    // listed instrument at the start of the session, before any trading
    // messages — but this scans the whole file so it's correct even if
    // that isn't true of a given file.
    static std::unordered_map<uint16_t, std::string>
    parse_stock_directory(const std::string& path);

    // Parse the full file, firing callbacks for each relevant message.
    // Returns number of messages processed (i.e. messages for which a
    // callback fired, after filtering).
    //
    // filter_locates: if non-empty, only fire callbacks for messages whose
    // stock_locate is in this set. Filtering is applied uniformly to EVERY
    // message type ('A'/'F'/'D'/'U'/'E'/'C'/'X') via each message's own
    // stock_locate field — there is no reliance on order_ref lookups
    // happening to miss for other instruments' orders (that was the old,
    // implicit, unverified filtering mechanism this replaces).
    // Empty set (default) means no filtering — fire everything.
    static size_t parse_file(const std::string& path,
                             const ParserCallbacks& cb,
                             const std::unordered_set<uint16_t>& filter_locates = {});
};
