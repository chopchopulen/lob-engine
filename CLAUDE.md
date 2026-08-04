# lob-engine

Benchmarked C++17 Nasdaq ITCH 5.0 limit order book reconstruction engine. Its
benchmark numbers (BM_FullPipeline: 16.93ns p50 / 17.17ns mean **per operation**;
~19.5M msg/s end-to-end; 268,744,780 ITCH message frames) are on the user's resume
— **nothing about them may silently regress**.

`docs/FINAL_NUMBERS.md` is the single source of truth for every number in this
project; `bench/BASELINE.md` is the source for benchmark methodology and raw
measurements. **No number goes on a resume, in a README, or into a commit message
unless it appears in FINAL_NUMBERS.md first.**

RETIRED — never cite: the "226M messages / 7.8M msg/s / 29.0 s" end-to-end claim
(the parser had no ticker filtering at that commit), all pre-harness-fix latency
figures (41/42/83/84/125/166/167/208/209 ns — tick-quantized), p99/p999 (cv 27–97%
and 18–108% on an unpinned host), and the "BVC ~6x worse than Lee-Ready" framing
(circular via the 'E'-tautology).

Per-op timing on this platform is tick-quantized (Apple Silicon ~41.667ns hardware
timer resolution); reported percentiles are grouped-batch (128-op window), not
single-operation percentiles — see BASELINE.md for full methodology.

**Per-operation ≠ per-message.** 16.93 ns is one book op on a synthetic in-memory
workload. End-to-end is ~54.6 ns/message (268,744,780 frames / 14.68 s). The gap is
parse + dispatch + bookkeeping and has never been decomposed. Do not conflate them.

## Rules

- Every performance-affecting change requires a before/after measurement against
  `bench/BASELINE.md`, captured with the exact reproduction command documented there.
  A change with no measurement attached is not done.
- Correctness of matching/book-reconstruction semantics is never traded for speed.
  If a perf win requires cutting a correctness corner, stop and ask — don't do it
  silently.
- The audit agents (`matching-correctness-auditor`, `itch-parser-auditor`,
  `perf-analyst`, `adversarial-reviewer`) are **read-only until the user says
  otherwise**. They build and run tests/benchmarks, but do not edit source. Only
  `builder` edits code, and only on items the user has explicitly approved.
- Build via CMake (`cmake -DCMAKE_BUILD_TYPE=Release ..`, requires Homebrew
  `google-benchmark`) for tests/benchmarks, or `make` for just the `lob_engine`
  binary. See `bench/BASELINE.md` for the exact audit build command.
- Raw ITCH `.bin` data files are not checked in (5-12GB each); `data/` only has
  derived CSVs. Every end-to-end throughput number requires a real ITCH file (via
  `download_itch.sh`) to reproduce. The current figures (~19.5M msg/s, 14.68 s avg,
  1.88x locate-scan early-exit) were measured on the 2019-12-30 main feed,
  8,251,407,909 bytes — see `bench/BASELINE.md` "Real end-to-end measurement".
- `scripts/validate_book.py` runs on every data regeneration and asserts positive,
  non-crossed, sub-dollar spreads. Do not disable it. A `uint32_t` underflow
  corrupted 99.6–100% of reconstructed rows for ~3 months and went undetected
  because the resulting OFI R² landed inside the published literature range.

See `.claude/agents/` for the full agent hierarchy and `audit/FINDINGS.md` for the
current ranked audit report.
