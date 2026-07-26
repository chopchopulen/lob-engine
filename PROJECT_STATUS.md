# Project Status (2026-07-24)

Single source of truth for what's currently claimable, retired, unverified, and open across
the whole project. Detail lives in `bench/BASELINE.md` (perf) and `results/OFI_STUDY.md`
(research); this file is the index.

> **⚠ All OFI/research numbers below are provisional as of 2026-07-24.** The `data/` CSVs they
> were computed from were found to be stale and corrupted (`quoted_spread` values in the
> hundreds of thousands of dollars). **Root cause confirmed 2026-07-24 by direct reproduction**
> (an earlier claim — a 4-byte vs 8-byte stock-field bug misaligning the `price` read — was
> checked against the commit diff, found unsupported, and retracted; it has now been replaced
> with a confirmed mechanism, not left open): the pre-Task-2 parser applied no ticker filtering
> to `'D'`/`'U'`/`'E'`/`'C'`/`'X'` messages, delivering every such message for every ticker to
> whichever single book was open. Running the actual pre-fix binary against a real Nasdaq BX
> sample reproduced the exact corruption (92.2% of MSFT's rows, 2.5% of AAPL's). This is fixed
> in current code by this session's Task 2 (`stock_locate` filtering, commit `3253443`),
> confirmed by replaying the same real file through today's code with zero corrupted rows. See
> `results/OFI_STUDY.md` "Blocker 1" for the full reproduction. This affects every research
> number in this file, the OFI study, and README's Research Results section regardless of when
> a given file was generated relative to the fix. Perf/benchmark numbers are unaffected
> (unrelated code path).
>
> **UPDATE, 2026-07-24 (later same day): regeneration has now been performed for one main-feed
> date (2019-12-30) and the contemporaneous OFI construction is validated on real data**
> (AAPL R²=0.62). `data/features_{AAPL,AMZN,NFLX,WDAY}.csv` are now real, verified-correct data
> — see "Claimable numbers" below. The multi-date *predictive* study (day-level split,
> walk-forward, HAC) has deliberately NOT been rerun — one date can't support a cross-date
> split — and remains provisional pending a decision on the full panel.

## Lesson learned (2026-07-24) — this is the most valuable thing in the whole audit

**Matching a published result is not a correctness check on a data pipeline; inspecting the
reconstructed intermediate object is.** AAPL's contemporaneous OFI→return R² (0.35–0.45)
landed in Cont/Kukanov/Stoikov's literature range and was initially read as validating the OFI
implementation. It didn't: `ofi` and `mid_price` were both derived from the same corrupted
book reconstruction, in the same process, from the same wrong `best_bid_price`/
`best_ask_price`. Two quantities computed from the same broken source will correlate the way
theory predicts regardless of whether the source is correct — that correlation lining up with
a published range was coincidence, not confirmation. The check that actually found the bug
(`results/OFI_STUDY.md` "Blocker 1", Step (c)) didn't compare against any downstream
statistical target at all — it printed the reconstructed `quoted_spread` and asked "is this
physically plausible," and found $429,492 on a $321 stock. That question a regression R² cannot
answer. This is now enforced automatically, not left as a step someone has to remember:
`scripts/validate_book.py` runs on every data regeneration (see below) and asserts exactly
that — positive, non-crossed, sub-dollar spreads — before any new data is trusted.

## Claimable numbers

| Number | Source | Required caveat |
|---|---|---|
| BM_FullPipeline mean: 17.17 ns | `bench/BASELINE.md` (Phase 3 table) | Grouped-batch methodology (128-op window), Apple Silicon, single core |
| BM_FullPipeline p50: 16.93 ns | `bench/BASELINE.md` | Same as above |
| Per-benchmark mean/p50 for all 8 benchmarks | `bench/BASELINE.md` Phase 3 table | cv ≤2.81% across 8 reps — stable, safe to cite single-sample |
| Pool-allocator fix: -28% to -54% mean latency reduction (5 of 6 op-touching benchmarks) | `bench/BASELINE.md` Phase 4 | ExecuteOrder correctly shows ~0% (never touches `orders_` insert/erase) — not a partial fix |
| Whole-file parse throughput: 18.4-18.8M msg/s, 263.24M messages, real main-feed data (2019-12-30) | `bench/BASELINE.md` "End-to-end throughput" | Parse-only (no book/feature reconstruction); replaces the retired 226M/7.8M end-to-end claim, not a like-for-like number — see caveat in source |
| Real end-to-end throughput: ~19.0-19.7M msg/s, full parse+book+feature pipeline, real main-feed data (2019-12-30, 5 study tickers, Apple M3 Pro) | `bench/BASELINE.md` "Real end-to-end measurement (corrected)" | An earlier draft of this figure ("~18.8k msg/s") was a mismatched-denominator artifact (filtered-match-count numerator over full-file-scan-time denominator) — diagnosed, confirmed against the code, and corrected, not just re-labeled. `main.cpp`'s timer also fixed to cover the whole run (was excluding `parse_stock_directory`'s own full-file scan, undercounting cost ~2x). Distinct from the retired 226M/7.8M claim, not a correction of it. Also found: the double full-file scan (locate resolution + parse) may be unnecessary — all `'R'` messages sit in the first 0.0044% of the file; early-exit proposed, not yet implemented |

**Contemporaneous OFI construction validation is now claimable — the first real research
result this project has had:**

| Number | Source | Required caveat |
|---|---|---|
| AAPL contemporaneous OFI→return R²=0.62 (2019-12-30, regular session) | `results/OFI_STUDY.md` "Real construction validation" | Single date, main-feed, verified-correct data (`scripts/validate_book.py` passed). Construction validation only — not a predictive/tradeable claim |
| AMZN R²=0.33, ETSY R²=0.09, NFLX R²=0.29, WDAY R²=0.16 (same date) | Same | Clean liquidity gradient consistent with theory (ETSY = smallest-cap, lowest R²); same single-date caveat |

**7-date main-feed panel assembled (2026-07-24), analyzed (2026-07-25).**
`data/panel_{AAPL,AMZN,ETSY,NFLX,WDAY}.csv` — 2019-01-30, 03-27, 07-30, 08-30, 10-30, 12-30,
2020-01-30, all 35 date×ticker combinations passed validation, provenance-stamped. AAPL's
price-scale flag resolved as organic 2019 appreciation, not a split (see `results/OFI_STUDY.md`
"Multi-date panel assembled").

**Contemporaneous OFI, full 7-date panel — liquidity gradient confirmed, AAPL highest, ETSY
lowest, consistent with the single-date table:**

| Ticker | Contemporaneous R² (7-date pooled) |
|---|---|
| AAPL | 0.5579 |
| AMZN | 0.3361 |
| NFLX | 0.3003 |
| WDAY | 0.2436 |
| ETSY | 0.1899 |

**Predictive OFI → 1s fwd return, cross-regime split (train = 4 earliest dates, test = 3
latest, ~2 months apart — not walk-forward): near-zero out-of-sample R² for all 5 tickers,
a legitimate finding, not a data-quality artifact.** R²_out ranges -0.0077 to +0.0069 across
15 ticker×test-date cells (several negative); every coefficient is HAC(5)-significant
(p<0.05, mostly p<0.0001) purely from sample size (N=35k-88k per ticker) — explicitly labeled
economically negligible for all 5 tickers, not read as a tradeable signal. Full table:
`results/OFI_STUDY.md` "OFI predictive study on the clean panel (2026-07-25)".

| Number (provisional, do not cite) | Source | Why not claimable |
|---|---|---|
| AAPL predictive OFI→return R²_out ≈ 0 (Step 2F, day-level split) | `results/OFI_STUDY.md` Step 2F | Computed on since-found-stale data — superseded by the 2026-07-25 cross-regime study above, not recomputed under this methodology |
| AMZN predictive OFI→return R²_out ≈ 0.3%, HAC(5) p<0.0001 (Step 2F) | `results/OFI_STUDY.md` Step 2F | Same — superseded; the 2026-07-25 clean-panel AMZN figure is R²_out≈0.6% pooled, still economically negligible |

## Retired numbers

| Number | Why retired | Where superseded |
|---|---|---|
| All pre-harness-fix latency figures (41/42/83/84/125/166/167/208/209/250 ns) | Tick-quantized: Apple Silicon's ~41.667ns hardware timer measured the clock, not the engine | `bench/BASELINE.md` SUPERSEDED section |
| README p99/p999 latency figures (22.46 ns / 45.90 ns) | Not retired for being wrong — pulled for being unmeasurable on this host: p99 cv 27–97% on 6/8 benchmarks, p999 cv 18–43% on all 8, dominated by OS scheduling noise, not engine behavior | `bench/BASELINE.md` p50/p99 stability table |
| README `items_per_second` throughput-derived latency table | Derived from the tick-quantized old harness; not re-derived from new numbers because that would not be a measurement | (removed, not replaced — see `bench/BASELINE.md` for why no substitute is offered) |
| OFI Iteration 1 within-day 70/30 split numbers (AAPL R²_out=-0.18%, AMZN R²_out=+0.07%) | Chronological split within a single day crosses the intraday U-shape regime boundary; 58% of the underlying data was extended-hours (pre/post-market), not disclosed in the original write-up. **Additionally and separately retired**: computed on data now known to be stale (see Unverified/Blocked section) | `results/OFI_STUDY.md` SUPERSEDED section |
| This session's own 2026-07-24 OFI re-audit numbers (day-level split, walk-forward, HAC) | Also computed on the same stale `data/` CSVs — the methodology fix was real, the numbers inherited the same underlying data problem | `results/OFI_STUDY.md` "Blocker 1" section |
| **226M messages / 7.8M msg/s / 29.0s end-to-end throughput** (`README.md`) | Tested for the first time 2026-07-25 against a real main-feed file (2019-12-30) and found unreproducible: the claim conflates whole-file scope with full per-instrument book reconstruction, and this codebase's `OrderBook` reconstructs one instrument at a time — no code path processes every instrument's messages through book reconstruction simultaneously, so "226M messages through the full pipeline" cannot correspond to any measurement of this code. Not corrected to a new single number; replaced with two separately-scoped real numbers (whole-file parse-only throughput, and single-ticker full-pipeline throughput) — see below | `bench/BASELINE.md` "End-to-end throughput" section (RETIRED verdict), `README.md` |

## Unverified / blocked

(empty — the only entry here, the 226M/7.8M end-to-end figure, was resolved 2026-07-25; see Retired numbers above)

**Regenerated for all 7 available main-feed dates, all 5 study tickers — 35/35 combinations
promoted, zero hard failures.** Dates: 2019-01-30, 03-27, 07-30, 08-30, 10-30, 12-30, and
2020-01-30 (all confirmed-downloadable main-feed dates per the Task 1 inventory — not a
continuous run). Each date/ticker went through the full locked pipeline
(`regenerate_data.sh` → `validate_book.py` raw-quote validation → `promote_data.sh`) and was
archived as `data/features_{TICKER}_{MMDDYYYY}.csv`. `data/panel_{AAPL,AMZN,ETSY,NFLX,WDAY}.csv`
are now assembled from all 7 dates with an explicit `date` column, provenance-stamped — no
longer stale. One flag found (AAPL price-scale, see `results/OFI_STUDY.md` "Multi-date panel
assembled"), not yet resolved. No predictive analysis has been run on this panel.

**Root cause of the original corruption is confirmed**: every previously-stale CSV in `data/`
predated this session's Task 2 (`stock_locate` filtering, commit `3253443`), which is the
actual fix — not the earlier-claimed, since-retracted item-4 stock-field bug. Before Task 2,
the parser delivered every `'D'`/`'U'`/`'E'`/`'C'`/`'X'` message for every ticker to whichever
single book was open, unfiltered; confirmed by reproducing the exact corruption running the
pre-fix binary against real Nasdaq BX data, and confirmed fixed by running today's code against
the same file with zero corrupted rows (`results/OFI_STUDY.md` "Blocker 1").

**Regeneration is now a two-step, staging-gated pipeline (2026-07-24) — never a direct write
to `data/` by default.** This exists because testing the regeneration script against a real
`data/` path during this same work session overwrote the real (if stale) `data/features_AAPL.csv`
with 3 rows of test output — unrecoverable, since `data/` is gitignored and untracked, and no
Time Machine/filesystem backup existed. The loss itself was low-cost (the file was already
known-corrupted and scheduled for replacement; `data/features_AMZN.csv` and every `panel_*.csv`
carry the identical corruption and were unaffected), but the *process gap* that allowed it —
nothing stood between "testing a script" and "overwriting real data" — is fixed structurally,
not just by being more careful next time:

```bash
# Step 1 — generate + stamp + validate into data/staging/ ONLY (never touches data/):
scripts/regenerate_data.sh <file.NASDAQ_ITCH50> AAPL AMZN ETSY NFLX WDAY

# Step 2 — explicit, separate promotion step. Re-validates staging immediately before
# copying (does not trust step 1's result is still current) and refuses to promote
# ANY file if ANY file fails, so a partially-bad batch can't overwrite data/:
scripts/promote_data.sh AAPL AMZN ETSY NFLX WDAY
# or: scripts/promote_data.sh --all

# Direct-to-data/ writes still exist (regenerate_data.sh --force <file> <TICKER>...) but
# require that explicit flag on the command line every time — no config/env equivalent.
```

Every generated CSV is stamped with a provenance sidecar (`scripts/provenance.py`) — a
`<file>.csv.meta` JSON file recording the exact commit hash that produced it and a UTC
timestamp. Chosen over a CSV header line (would require every reader to know to skip it) or a
filename-embedded hash (unwieldy, breaks every consumer path on every commit). Any script that
loads a features CSV (`scripts/analysis.py`) calls `verify_provenance()` first and **raises**
(does not warn) if the stamp is missing or doesn't match current `HEAD` — naming both the
stamped and current commit hashes in the error. Proven with a dedicated test
(`scripts/test_provenance.py` — passes against a correct stamp, fails loudly against a missing
stamp, fails loudly against a deliberately mismatched one, asserting both hashes appear in the
error message).

To regenerate for real once a raw file is available: run the two-step pipeline above, then
rerun `results/OFI_STUDY.md`'s methodology (day-level split, HAC(5)) against the fresh,
promoted data — the methodology itself does not need to change, only the input data. No
automated multi-day panel builder exists yet; `regenerate_data.sh` handles one raw file (one
day) at a time, so rebuilding a `panel_*.csv` means running it once per day and concatenating.

## Open items (not investigated further — flagged, not resolved)

- **ASan/UBSan build hang.** Reproduces on pre-item-3 code too, so not caused by the
  pool-allocator fix. Hypothesis (unconfirmed): macOS ASan environment pathology. Logged in
  `audit/FINDINGS.md`; not investigated per explicit instruction to log-only.
- **Tail percentiles (p99, p999) are not reliably measurable without core pinning** on this
  unpinned, multi-tenant host. Documented in `bench/BASELINE.md` and `README.md`.
- **p99 cv spread across benchmarks has no established mechanism.** ReplaceOrder cv=1.64% vs
  DeleteOrder cv=96.93%, with no clean relationship to op cost, op type, or anything else
  checked so far. Recorded as an observation only — no mechanism should be assumed or cited
  until it's actually tested.
- ~~AMZN's contemporaneous OFI→return R² unexplained, root cause of ticker-asymmetric
  corruption unknown~~ — **resolved 2026-07-24 by direct reproduction** (superseding an
  intermediate retraction that left this genuinely open): the pre-Task-2 parser delivered every
  `'D'`/`'U'`/`'E'`/`'C'`/`'X'` message, for every ticker, to whichever single book was open,
  with no ticker filtering at all — confirmed by running the actual pre-fix binary against real
  BX data (92.2% of MSFT's rows corrupted, 2.5% of AAPL's). Confirmed fixed in current code
  (Task 2, `stock_locate` filtering, commit `3253443`) via the same real-data test. The
  *precise* reason `order_ref` collisions occur this frequently on real data (venue-specific
  numbering, ID reuse, or something else) is not further characterized, and AAPL's
  plausible-looking R² is still never valid evidence of a correct pipeline regardless of
  mechanism (see "Lesson learned" above) — but the corruption mechanism itself is no longer
  open. See `results/OFI_STUDY.md` "Blocker 1" for the full reproduction.
- ~~Parser relied on `order_ref` global uniqueness across tickers, unverified against real
  data~~ — **fixed 2026-07-24**: `stock_locate` is now parsed from the ITCH Stock Directory
  (`'R'`) messages and used to filter every message type (`'A'`/`'F'`/`'D'`/`'U'`/`'E'`/`'C'`/
  `'X'`), not just Add Order. The old order_ref-lookup-miss mechanism is removed. Covered by 3
  new tests in `test/test_order_book.cpp`, including one that deliberately constructs two
  tickers sharing the same `order_ref` and proves cross-contamination no longer occurs. No
  regression in `bench/BASELINE.md` mean/p50 (unaffected code path — the microbenchmarks
  exercise `OrderBook`/`FeatureEngine` directly, never `itch_parser.cpp`).
- ~~ETSY not promoted to `data/` for 2019-12-30~~ — **resolved 2026-07-24**: `validate_book.py`
  now scopes its absurd-spread/degenerate-spread hard fails to regular-session hours only
  (09:30–16:00 ET); pre-market/post-market rows are reported informationally, never block
  promotion. All 5 tickers (including ETSY) now pass and are promoted. See
  `results/OFI_STUDY.md` "Degenerate-quote characterization."
- **VPIN / Lee-Ready work: explicitly not started**, per current instruction.

## Repo state

- Engine changes (`stock_locate` filtering), test additions, provenance tooling, and updated
  docs committed and pushed this turn, per explicit instruction. `.gitignore`'s `build_audit/`
  line remains a pre-existing uncommitted one-line addition from earlier in this session — not
  part of this batch, left as-is.
- `main` pushed to `origin/main`.
- **Incident, disclosed:** `data/features_AAPL.csv` (untracked, gitignored, never in git
  history) was overwritten with 3 rows of test output while testing the regeneration script
  against a real path instead of a scratch path. Unrecoverable — no backup existed. Left as-is
  per explicit instruction (the file was already known-stale/corrupted and scheduled for
  replacement; no other file was affected). The regeneration pipeline was restructured
  (staging + explicit promotion, see Unverified/Blocked above) specifically so this class of
  mistake can't happen again.
