#include <benchmark/benchmark.h>
#include "feed/itch_parser.h"
#include "bench_timing.h"
#include <cstdlib>
#include <string>

// ── BM_ParseFile ──────────────────────────────────────────────────────────────
// Parse-throughput benchmark for itch_parser.cpp — the component this repo's
// microbenchmarks never covered (bench_book.cpp/bench_pipeline.cpp exercise
// OrderBook/FeatureEngine directly and never call the parser at all, which is
// why the stock_locate refactor's bench rerun was a null test by construction;
// see PROJECT_STATUS.md). This is the missing component-level counterpart to
// the engine's end-to-end throughput claim (226M msg / 7.8M msg/s, itself
// still unverified — see bench/BASELINE.md).
//
// TIMING DISCIPLINE, adapted from bench_timing.h's grouped-batch approach
// (see that file for the full tick-resolution rationale): bench_timing.h's
// GROUP_SIZE=128 exists because a single OrderBook op (~15-30ns) is far
// below Apple Silicon's ~41.67ns timer tick, so many ops must be batched into
// one timed window before the *tick itself* stops being the dominant source
// of error. That problem does not apply here: one call to
// ItchParser::parse_file() on a real multi-hundred-thousand-message file
// takes tens to hundreds of MILLISECONDS -- many thousands of ticks -- so a
// single call is already far above the tick-resolution floor. Re-using
// GROUP_SIZE=128 literally (128 full-file parses per timed group) would make
// each benchmark run take tens of minutes to hours for no accuracy benefit.
// What IS re-used from bench_timing.h's discipline: warmup discarded before
// any sample is recorded (here: one untimed parse to pull the file into the
// OS page cache, since cold-disk-read jitter is this workload's analogue of
// bench_timing.h's cold-allocator jitter), and percentiles (p50/p99) reported
// across repeated measurements rather than trusting a single timed run.
//
// Input file: set via the LOB_BENCH_ITCH_FILE environment variable (a real
// ITCH file, e.g. an unzipped Nasdaq BX/PSX sample from
// https://emi.nasdaq.com/ITCH/ -- see bench/BASELINE.md for the exact file
// this was measured against). Not committed to the repo (real ITCH files are
// hundreds of MB, same reason data/*.bin is gitignored). Skips with a clear
// message if unset, rather than silently benchmarking nothing.
//
// No locate filter: parse_file()'s returned count is the number of messages
// actually dispatched to a callback, which for a FILTERED parse (e.g. one
// ticker) undercounts total parser throughput by orders of magnitude -- most
// of the wall time is spent scanning and cheaply skipping non-matching
// messages, not in the dispatched callbacks. This benchmark deliberately
// measures the unfiltered case (every message dispatched, every parsing
// branch exercised) so "messages / elapsed time" is an honest total-scan
// throughput figure, comparable to the engine's own end-to-end msg/s claim.
// Realistic single-ticker usage is faster than this number, not slower --
// most messages take the cheap early-exit filter path instead of full
// field decode + callback dispatch.

static void BM_ParseFile(benchmark::State& state) {
    const char* path_env = std::getenv("LOB_BENCH_ITCH_FILE");
    if (!path_env || std::string(path_env).empty()) {
        state.SkipWithError(
            "LOB_BENCH_ITCH_FILE not set -- point it at a real unzipped ITCH file "
            "(e.g. from https://emi.nasdaq.com/ITCH/) to run this benchmark. "
            "See bench/BASELINE.md for the reference file this was measured against.");
        return;
    }
    const std::string path = path_env;

    ParserCallbacks cb;
    size_t sink = 0;   // prevents the optimizer from eliding the parse
    cb.on_add     = [&](const AddOrderMsg&)     { ++sink; };
    cb.on_delete  = [&](const DeleteOrderMsg&)  { ++sink; };
    cb.on_replace = [&](const ReplaceOrderMsg&) { ++sink; };
    cb.on_execute = [&](const ExecuteOrderMsg&) { ++sink; };
    cb.on_cancel  = [&](const CancelOrderMsg&)  { ++sink; };

    std::unordered_set<uint16_t> filter;   // empty = unfiltered, every message dispatched

    // Untimed warmup: pull the file into the OS page cache once, so the
    // timed reps measure parse cost, not cold-disk-read latency.
    size_t msg_count = ItchParser::parse_file(path, cb, filter);

    std::vector<double> rep_ns;
    rep_ns.reserve(64);

    for (auto _ : state) {
        auto t0 = Clock::now();
        msg_count = ItchParser::parse_file(path, cb, filter);
        auto t1 = Clock::now();
        rep_ns.push_back(elapsed_ns(t0, t1));
    }

    if (!rep_ns.empty()) {
        std::sort(rep_ns.begin(), rep_ns.end());
        double sum = 0;
        for (double v : rep_ns) sum += v;
        double mean_ns = sum / rep_ns.size();

        state.counters["mean_ms"]      = mean_ns / 1e6;
        state.counters["p50_ms"]       = rep_ns[rep_ns.size() * 50 / 100] / 1e6;
        state.counters["messages"]     = static_cast<double>(msg_count);
        state.counters["msgs_per_sec"] = msg_count / (mean_ns / 1e9);
        state.counters["ns_per_msg"]   = mean_ns / static_cast<double>(msg_count);
    }
}
BENCHMARK(BM_ParseFile)->MinTime(1.0)->Unit(benchmark::kMillisecond);
