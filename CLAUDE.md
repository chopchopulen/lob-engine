# lob-engine

Benchmarked C++17 Nasdaq ITCH 5.0 limit order book reconstruction engine. Its
benchmark numbers (BM_FullPipeline: 16.93ns p50 / 17.17ns mean, ~7.8M msg/s,
226M+ ITCH messages replayed) are on the user's resume — **nothing about them
may silently regress**.

`bench/BASELINE.md` is the single source of truth for benchmark numbers. Per-op
timing on this platform is tick-quantized (Apple Silicon ~41.667ns hardware
timer resolution); reported percentiles are grouped-batch (128-op window), not
single-operation percentiles — see BASELINE.md for full methodology.

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
  derived CSVs. The 226M-message/7.8M-msg/s end-to-end throughput claim requires a
  real ITCH file (via `download_itch.sh`) to reproduce — it was not verified in the
  most recent audit for that reason.

See `.claude/agents/` for the full agent hierarchy and `audit/FINDINGS.md` for the
current ranked audit report.
