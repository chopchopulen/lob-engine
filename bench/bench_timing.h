#pragma once
#include <benchmark/benchmark.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Grouped-batch timing.
//
// WHY THIS EXISTS: Apple Silicon's mach_absolute_time (what
// std::chrono::high_resolution_clock wraps on macOS) ticks at 24MHz —
// mach_timebase_info numer=125/denom=3, i.e. one tick = 41.666...ns. A single
// order-book op costs 30-80ns, so timing ONE op with one now()-before/
// now()-after pair measures a duration of 0-2 ticks: the result is quantized
// to multiples of ~41.67ns (0, 42, 83, 125, ...) and says almost nothing
// about the actual op cost. This is exactly the pattern the pre-fix harness
// showed (p50/p99 values were all integer multiples of the tick; ExecuteOrder
// reported p50=0ns; two builds with a genuinely different op cost round-
// tripped to the identical 41/42/84ns bucket). There is no userspace cycle
// counter on Apple Silicon (PMCCNTR_EL0 is EL1-restricted) to fall back on.
//
// FIX: time a GROUP of GROUP_SIZE consecutive ops in one timed region, divide
// by GROUP_SIZE. A group takes GROUP_SIZE * (30-80ns) ≈ several microseconds
// — thousands of ticks — so the tick-quantization error on the GROUP
// measurement is ~1/N relative (≈0.3-0.8% for N=128), not ~100% relative like
// timing one op. Repeating this over many groups gives both:
//   - a precise MEAN per-op cost (mean of the per-group means — exact, since
//     all groups are the same size, this equals the true overall mean)
//   - a DISTRIBUTION of group-mean costs, from which we report p50/p99/p999
//
// WHAT THE REPORTED PERCENTILES ARE — AND ARE NOT:
//   p50_ns / p99_ns / p999_ns below are percentiles of "mean cost per op,
//   averaged over a GROUP_SIZE-op window". They are NOT percentiles of a
//   single operation's latency distribution. If one op in a 128-op group is
//   a 10x-slower outlier (e.g. a page fault, an allocator rehash, an
//   overflow-map promotion), it moves that group's mean by ~1/128th of the
//   outlier's excess cost — a true single-op tail event is heavily damped,
//   not eliminated, by this method. This is the tradeoff for operating above
//   the timer's resolution floor: coarser tail granularity in exchange for a
//   trustworthy (non-quantized) number at all. Batched timing (option a) was
//   rejected as the sole method because it gives a mean only, no distribution
//   at all; Google Benchmark's own auto-scaling (option c) times the whole
//   loop body the same way ours does and doesn't solve the single-op problem
//   either — grouping is the mechanism regardless of who drives the loop.
//
// GROUP_SIZE=128 chosen so a group's expected duration (128 * ~50ns ≈
// 6400ns) is roughly 150x the 41.67ns tick, keeping per-group quantization
// error comfortably under 1%. WARMUP_GROUPS=500 (64,000 ops) are executed
// and discarded before any sample is recorded, to exclude cold-cache/
// cold-allocator effects from the reported distribution.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t GROUP_SIZE     = 128;
static constexpr uint64_t WARMUP_GROUPS  = 500;

using Clock = std::chrono::high_resolution_clock;

// Returns elapsed time as a double in nanoseconds (sub-tick precision in the
// representation; the underlying clock's actual resolution is the ~41.67ns
// tick documented above — dividing a many-tick duration by GROUP_SIZE is what
// recovers sub-tick information about the MEAN, not the clock itself).
inline double elapsed_ns(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// group_means_ns: one sample per non-warmup group, each sample already
// divided by GROUP_SIZE (i.e. "mean ns per op, this group").
inline void report_grouped_percentiles(benchmark::State& state,
                                        std::vector<double>& group_means_ns) {
    if (group_means_ns.empty()) return;
    std::sort(group_means_ns.begin(), group_means_ns.end());

    const double sum = std::accumulate(group_means_ns.begin(), group_means_ns.end(), 0.0);
    const double mean = sum / static_cast<double>(group_means_ns.size());

    state.counters["mean_ns"] = mean;
    state.counters["p50_ns"]  = group_means_ns[group_means_ns.size() * 50  / 100];
    state.counters["p99_ns"]  = group_means_ns[group_means_ns.size() * 99  / 100];
    state.counters["p999_ns"] = group_means_ns[group_means_ns.size() * 999 / 1000];
    state.counters["groups"]  = static_cast<double>(group_means_ns.size());

    // items = individual ops, not groups — GROUP_SIZE ops were processed per
    // recorded group, plus WARMUP_GROUPS*GROUP_SIZE discarded warmup ops.
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                             static_cast<int64_t>(GROUP_SIZE));
}
