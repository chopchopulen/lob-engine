# lob-engine

A high-performance C++17 limit order book reconstruction engine for Nasdaq ITCH 5.0 binary feeds, with microstructure feature extraction and out-of-sample alpha research.

## Architecture

The pipeline is organized into four layers:

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1 — Feed Parser   (include/feed/, src/feed/)     │
│  Reads raw ITCH 5.0 binary; emits typed message structs │
├─────────────────────────────────────────────────────────┤
│  Layer 2 — Order Book    (include/book/, src/book/)     │
│  Maintains bid/ask price levels as flat sorted arrays   │
│  (replaced std::map; –11% pipeline latency at depth 20) │
├─────────────────────────────────────────────────────────┤
│  Layer 3 — Feature Engine (include/features/, src/)     │
│  Computes OFI, bid-ask spread, and trade imbalance      │
│  on 1-second intervals; writes CSV for downstream use   │
├─────────────────────────────────────────────────────────┤
│  Layer 4 — Analysis      (scripts/analysis.py)          │
│  OLS regression of normalized OFI against future price  │
│  changes; reports in-sample and out-of-sample R²        │
└─────────────────────────────────────────────────────────┘
```

## Dependencies

- C++17-compatible compiler (Apple Clang ≥ 12, GCC ≥ 9)
- GNU Make
- Python 3 with `pandas`, `numpy`, `statsmodels` (for `scripts/analysis.py`)

No third-party C++ libraries are required.

## Build

```bash
make          # produces ./lob_engine
make clean    # remove build/ and binary
```

The Makefile uses `-O2 -std=c++17` and auto-tracks header dependencies via `-MMD -MP`, so incremental builds only recompile what changed.

## Usage

```bash
./lob_engine <itch_file.bin> <TICKER> <output.csv>
```

**Example:**

```bash
./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv
```

The engine will print a progress summary on exit:

```
Done.
  Messages processed:  226000000
  Trades:              1840312
  Feature rows:        23400
  Elapsed:             29.0s
  Throughput:          7.8M msg/s

Features written to: data/features_AAPL.csv
```

Then run the regression analysis:

```bash
python3 scripts/analysis.py data/features_AAPL.csv
```

## Benchmark Results

Measured on Apple M-series (arm64), single core, release build (`-O2`).

| Metric | Value |
|---|---|
| Messages processed | 226M |
| Throughput | 7.8M msg/s |
| Median per-message latency | 41 ns |
| p99 per-message latency | 84 ns |
| Pipeline latency reduction (flat array vs. `std::map`) | −11% at depth 20 |

The flat sorted-array book representation eliminates pointer-chasing and improves cache locality at the common case of 5–20 active price levels per side.

## Research Results

OFI (Order Flow Imbalance) regression against 1-second forward price changes, trained on the first 70% of the trading day, evaluated on the remaining 30%.

| Ticker | Out-of-sample R² |
|---|---|
| AMZN | 0.07% |
| AAPL | ~0% |

The AMZN signal is consistent with findings in Cont et al. (2014): OFI carries a statistically detectable predictive signal, but the effect is weaker on more liquid instruments where adverse selection is lower and signal decay is faster. AAPL's near-zero out-of-sample R² reflects its higher liquidity regime.

## Project Structure

```
lob-engine/
├── include/
│   ├── feed/
│   │   ├── itch_parser.h        # Parser interface and callback types
│   │   └── itch_types.h         # ITCH 5.0 message structs (AddOrder, Delete, etc.)
│   ├── book/
│   │   └── order_book.h         # OrderBook class — flat sorted-array price levels
│   └── features/
│       └── feature_engine.h     # FeatureEngine — OFI, spread, trade imbalance
├── src/
│   ├── feed/
│   │   └── itch_parser.cpp      # Binary ITCH 5.0 message dispatch loop
│   ├── book/
│   │   └── order_book.cpp       # Add / delete / replace / execute order logic
│   ├── features/
│   │   └── feature_engine.cpp   # Per-second feature aggregation and CSV writer
│   └── main.cpp                 # Entry point — wires parser → book → features
├── bench/
│   ├── bench_book.cpp           # Microbenchmark: order book operations
│   └── bench_pipeline.cpp       # End-to-end pipeline latency benchmark
├── test/
│   └── test_order_book.cpp      # Unit tests for order book correctness
├── scripts/
│   ├── analysis.py              # OLS regression and R² reporting
│   └── generate_test_data.py    # Synthetic ITCH message generator for testing
├── docs/                        # Design specs and implementation plans
├── Makefile
└── CMakeLists.txt
```

## Data

ITCH 5.0 binary files are available from the [Nasdaq Historical Data](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) portal. Files are on the order of 5–12 GB per trading day and are excluded from this repository via `.gitignore`.

Place downloaded files in `data/` before running.

## References

- Cont, R., Kukanov, A., & Stoikov, S. (2014). [The Price Impact of Order Book Events](https://doi.org/10.1093/jjfinec/nbt002). *Journal of Financial Econometrics*, 12(1), 47–88.
- Nasdaq (2019). [Nasdaq TotalView-ITCH 5.0 Specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf).
