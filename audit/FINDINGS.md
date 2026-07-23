# lob-engine Audit Findings — Phase 1 (read-only)

Captured 2026-07-23. Three parallel auditors (matching-correctness, ITCH-parser,
perf/benchmark-methodology) + one adversarial refutation pass (Opus). Every finding
below is CONFIRMED by the adversarial reviewer — nothing was refuted outright, but
severities/framing were adjusted where noted. Baseline numbers this audit is measured
against: `bench/BASELINE.md`. No engine code was edited during this audit.

Ranked by severity (high → low), correctness items first within a tier since they
can silently corrupt output, then performance.

---

## HIGH

### 1. Missing `'X'` (Order Cancel — partial) message type
- **File:** `src/feed/itch_parser.cpp` (message-type dispatch, only `'A'/'F'/'D'/'U'/'E'` handled)
- **Type:** correctness
- **Why it's a bug:** ITCH 5.0 `'X'` reduces a still-live order's remaining shares (partial cancel) without deleting it — distinct from `'D'` (full delete) and `'E'` (execution). Unhandled, so any order later touched by an `'X'` event keeps a stale (too-large) share count in the book forever, or until it's fully deleted/replaced. Framing is unaffected (message bytes are still consumed correctly before the type dispatch), only the semantic update is missing. Real Nasdaq feeds contain `'X'` heavily.
- **Fix:** add an `'X'` branch. Spec layout: type(1)@0, stock_locate(2)@1-2, tracking(2)@3-4, timestamp(6)@5-10, order_ref(8)@11-18, canceled_shares(4)@19-22 — 23 bytes total. Route `canceled_shares` through a shares-reduction path (extend/reuse `execute_order` semantics, or add a `cancel_shares` method to `OrderBook` that reduces the order without deleting it, distinct from `execute_order`'s trade-count semantics).

### 2. Missing `'C'` (Order Executed With Price) message type
- **File:** `src/feed/itch_parser.cpp`, same dispatch block
- **Type:** correctness
- **Why it's a bug:** `'C'` is functionally like `'E'` (reduces an order's shares via execution) but is used for executions away from the order's display price. Unhandled entirely — those executed shares are never deducted from the resting order, so the book silently overstates liquidity, and the execution never counts toward trade/feature stats. Nonzero frequency in real Nasdaq data.
- **Fix:** add a `'C'` branch. Spec layout: type(1)@0, stock_locate(2)@1-2, tracking(2)@3-4, timestamp(6)@5-10, order_ref(8)@11-18, executed_shares(4)@19-22, match_number(8)@23-30, printable(1)@31, execution_price(4)@32-35 — 36 bytes total. Route `executed_shares` through the same book-update path as `'E'`.

### 3. `orders_` unordered_map has no pool/arena — heap alloc/free on every hot-path op
- **File:** `include/book/order_book.h:107`; `src/book/order_book.cpp:138,148,158-159,172`
- **Type:** performance
- **Why it's real:** Verified directly — `sizeof(Order)==24`, and no `reserve()` call exists on `orders_` anywhere in the codebase (the only two `reserve()` calls in the file are on the result vectors in `bid_levels`/`ask_levels`, unrelated). `orders_` is a node-based `unordered_map`: every `add_order` is a hash insert (node alloc on miss), every `delete_order`/zero-fill `execute_order` is an erase (node dealloc), and `replace_order` does **both** (erase + insert = two allocator round-trips) at `order_book.cpp:158-159`. This plausibly explains a large share of the measured 41-84ns/op, and specifically explains `BM_ReplaceOrder`'s fatter p99 tail (125ns, cv=25.9% — notably worse than the other ops) in `bench/BASELINE.md`. `reserve()` alone won't fix this since book size is already stable during steady-state operation — the actual lever is a pool/arena or an open-addressing map (e.g. `absl::flat_hash_map`, or a hand-rolled slab + free-list keyed by index instead of pointer).
- **Fix:** replace `orders_` with an open-addressing map, or add an explicit slab/free-list pool for `Order` with `orders_` storing indices instead of allocating per-node. Must be re-benchmarked against `bench/BASELINE.md` before claiming any new number — this is flagged as the single highest-leverage, lowest-risk performance change available, and directly explains an unexplained tail in the current baseline.

---

## MEDIUM

### 4. `AddOrderMsg::stock` truncates ITCH's 8-byte ticker to 4 chars
- **File:** `include/feed/itch_types.h:58` (`char stock[5]`); `src/feed/itch_parser.cpp:69,93` (`strncpy(m.stock, stk8, 4)`)
- **Type:** correctness (latent — currently unreachable with supported tickers)
- **Why it's a bug:** The 8-byte ITCH ticker field is read correctly (`memcpy(stk8, msg+24, 8)`, offset/length verified against spec), but then explicitly truncated to 4 chars before being stored in `m.stock`. Any 5+ character ticker (e.g. `GOOGL`) gets silently truncated. **Confirmed not currently exploitable**: `main.cpp:128-130` has an existing comment acknowledging this exact limitation ("m.stock is truncated to 4 chars... Tickers longer than 4 chars (e.g. GOOGL) will not match. All currently supported tickers (AAPL, SPY, AMZN) are 4 chars or fewer."), and the single-ticker `filter_stock` path in the parser compares against the untruncated `stk8`, not the truncated `m.stock` — so filtering itself is unaffected. Only the dual-ticker routing mode in `main.cpp` (which reads `m.stock` from the callback) would silently drop a 5+ char ticker's orders (fails safe — no cross-contamination — but silently produces an empty book with no error).
- **Fix:** widen `AddOrderMsg::stock` to `char stock[9]` and copy the full stripped `stk8` instead of truncating to 4. Small, mechanical, zero perf cost (parse-time copy, not hot-path).

### 5. `replace_order` (and `add_order`) silently orphan share-count on order_ref collision
- **File:** `src/book/order_book.cpp:159` (also present identically at line 138 in `add_order`)
- **Type:** correctness (defensive gap, not reachable on a clean feed)
- **Why it's a bug:** `orders_[new_ref] = Order{...}` overwrites any existing live entry at `new_ref` without first removing its shares from the price-level aggregate — an orphaned/leaked share count that persists in the book's aggregate state. ITCH guarantees new-order-ref uniqueness on a genuine feed, so this shouldn't fire in practice — but a corrupted, truncated, or gapped replay (e.g. a dropped sequence-gap from upstream framing) could trigger silent aggregate corruption that's very hard to detect (no crash, just a quietly wrong book for the rest of the session).
- **Fix:** before the overwrite, check `orders_.count(new_ref)`; if present, call `remove_from_level` on the stale entry first (or assert/log in a debug build to surface feed corruption early).

---

## LOW / INFORMATIONAL

### 6. `'E'` (Order Executed) accepted with `msg_len` as low as 23, though spec size is always 31
- **File:** `src/feed/itch_parser.cpp:139`
- **Type:** correctness (defensive-coding nit, not memory-unsafe)
- Verified: the parser only ever reads offsets it has confirmed are within `msg_len` (never reads the unread `match_number` field at offset 23-30), so no out-of-bounds/stale-memory read occurs. But it silently accepts a malformed/impossibly-short `'E'` message rather than flagging it, which could mask upstream feed corruption. Optional: assert/log when `msg_len != 31` for `'E'` specifically.

### 7. `FlatLevels` header comment inaccurate — spans 3 cache lines, not "one or two"
- **File:** `include/book/order_book.h:34`
- **Type:** documentation only
- Verified: `sizeof(FlatLevels) == 164` bytes (`std::array<PriceLevel,20>` = 160B + `uint32_t size` = 4B), which spans 3 64-byte cache lines, not 1-2 as the comment claims. Pure doc fix, no functional impact.

### 8. Intra-level time priority is not tracked — informational, not a bug
- **File:** `include/book/order_book.h:44-56`, `src/book/order_book.cpp:44-89`
- **Type:** design note
- `FlatLevels` aggregates all orders at a price into a single `shares` total; individual order arrival order within a level isn't tracked beyond what's implicit in `orders_` (unordered). Confirmed this is correct for a pure book-*reconstruction* engine — ITCH always addresses mutations by `order_ref`, never by queue position, so nothing in the current public API (`top_of_book`/`bid_levels`/`ask_levels`) needs per-order ordering. Would only matter if a future feature needed to expose queue position or simulate fills locally.

### 9. D/E/U silent no-op on unknown `order_ref` — expected, not a bug
- **File:** `src/book/order_book.cpp:145,154-155,166-167`
- **Type:** design note / diagnosability gap
- Confirmed correct and unavoidable: ITCH's Delete/Execute/Replace messages carry no ticker symbol (only `order_ref`), so a single-ticker filtered replay will always see D/E/U references for *other* tickers' orders that were never added to `orders_` — those correctly no-op. The gap: this is indistinguishable from a genuine data/replay corruption for the *same* ticker. No counters/logging exist to separate the two cases. Not a book-correctness defect (the no-op is right either way), just an observability gap if gap-detection is ever wanted.

### 10. Benchmark methodology — reviewed thoroughly, no issues found
- **Files:** `bench/bench_book.cpp`, `bench/bench_pipeline.cpp`
- Confirmed sound on every axis checked: state is reset/held constant across iterations (no drift into overflow mid-run for "steady" fixtures), p50/p99 are computed from real sorted per-iteration `chrono` samples (not min/max mislabeled), and the "Failed to set thread affinity" warning is confirmed cosmetic — it only affects google-benchmark's *metadata* CPU-frequency estimate, not the manual `chrono`-based timing these benchmarks actually report. One accepted, already-documented limitation: `high_resolution_clock` has ~15ns granularity, visible as `p50_ns=0` on `BM_ExecuteOrder` — not a flaw, just a measurement floor.
- One coverage gap (not a defect): no benchmark exercises repeated promotion/demotion "boundary churn" at the flat-array/overflow-map edge (ranks 19-21), which does a `std::map` insert *and* erase per call and could plausibly be worse than either the always-in-flat or always-in-overflow fixtures currently measured. Worth adding if the fix loop ever touches `add_to_level`/`remove_from_level`.

---

## Unverified resume claim (not a finding — a gap)

**226M messages / 7.8M msg/s / 29.0s end-to-end throughput (README.md) is NOT reproducible on this machine** — no raw ITCH `.bin`/`.NASDAQ_ITCH50` file is present locally (`data/` only has derived CSVs; `download_itch.sh` pulls 5-12GB/day files from Nasdaq's public archive, none cached). The microbenchmark numbers (41-84ns/op) don't cleanly prove or disprove this claim either way — real single-ticker replay is dominated by mostly-miss D/E/U lookups for other tickers (cheap, not exercised by the always-hit microbenchmarks) plus per-message parse overhead paid regardless of ticker, so a naive ceiling calculation from the microbenchmarks is not a valid disproof. **Action needed:** download one real ITCH day file and rerun `./lob_engine <file> <TICKER> <output.csv>` to actually verify this number; update `bench/BASELINE.md` once done.

---

## Summary table

| # | Finding | Severity | Type | Verdict |
|---|---|---|---|---|
| 1 | Missing `'X'` Order Cancel handling | High | Correctness | CONFIRMED |
| 2 | Missing `'C'` Order Executed w/ Price handling | High | Correctness | CONFIRMED |
| 3 | `orders_` unordered_map, no pool/arena | High | Performance | CONFIRMED |
| 4 | `stock[5]` truncates 8-byte ticker to 4 | Medium | Correctness (latent) | CONFIRMED |
| 5 | order_ref collision orphans share count | Medium→Low | Correctness (defensive) | CONFIRMED |
| 6 | `'E'` accepted below spec's 31-byte size | Low | Correctness (nit) | CONFIRMED |
| 7 | FlatLevels cache-line comment inaccurate | Low | Docs | CONFIRMED |
| 8 | No intra-level time priority tracked | Informational | Design note | CONFIRMED (not a bug) |
| 9 | D/E/U silent no-op ambiguity | Informational | Diagnosability | CONFIRMED (not a bug) |
| 10 | Benchmark methodology | — | Methodology | CONFIRMED sound |
| — | 226M msg / 7.8M msg/s claim | — | Unverified | Needs real ITCH file |

Baseline reference: `bench/BASELINE.md`. Next step: user picks which items to fix; only then does the fix loop start (builder → test-runner + perf-analyst re-measure → adversarial-reviewer confirms no regression vs BASELINE.md → repeat).
