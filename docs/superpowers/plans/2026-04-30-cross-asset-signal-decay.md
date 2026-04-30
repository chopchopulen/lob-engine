# Cross-Asset OFI & Signal Decay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add dual-ticker single-pass mode to the C++ engine, a cross-asset OFI regression (SPY→AAPL, lag-1 and lag-2), and a multi-frequency signal decay analysis with PNG output.

**Architecture:** `main.cpp` gains an `argc==5` branch that runs two `(OrderBook, FeatureEngine)` pairs in one parser pass, routing via an `order_ref→ticker` map maintained in callbacks; the combined CSV is merged post-parse by second-floor bucket. Both new research functions live in `analysis.py` and are gated behind `--cross` / `--decay` CLI flags.

**Tech Stack:** C++17, Python 3, pandas, sklearn, matplotlib

---

## File Map

| File | Change |
|---|---|
| `src/main.cpp` | Add `argc==5` dual-ticker branch with routing map and combined CSV writer |
| `scripts/analysis.py` | Add `cross_asset_ofi()`, `signal_decay()`, update `main()` CLI |

No other files are modified.

---

## Task 1: Dual-Ticker Mode in `main.cpp`

**Files:**
- Modify: `src/main.cpp`

### Background

The parser's `filter_stock` param currently handles ticker filtering. `'D'`/`'U'`/`'E'` messages carry no stock field — only an `order_ref`. So when `filter_stock=""` (no filter), callbacks fire for every message, and `main.cpp` must route by maintaining an `unordered_map<uint64_t, uint8_t> order_route` (populated on `on_add`, consulted on D/U/E).

After parsing, both feature engines have independent row vectors. The combined CSV is assembled by bucketing each row's `timestamp_ns` to `ts / 1'000'000'000` (second floor) and writing only seconds where both tickers produced a row.

- [ ] **Step 1: Add new headers to `src/main.cpp`**

Add these includes after the existing three:

```cpp
#include <iostream>
#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>
#include "feed/itch_parser.h"
#include "book/order_book.h"
#include "features/feature_engine.h"
```

- [ ] **Step 2: Replace the `argc < 4` usage check and add the dual-ticker branch**

Replace the entire `main()` body with the following. The single-ticker path is identical to the current implementation — copy it verbatim then add the new `else` block.

```cpp
int main(int argc, char* argv[]) {
    // ── Usage ──────────────────────────────────────────────────────────────
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <itch_file> <TICKER> <output.csv>\n"
                  << "  " << argv[0] << " <itch_file> <TICKER1> <TICKER2> <combined.csv>\n";
        return 1;
    }

    const std::string itch_file = argv[1];

    // ── Single-ticker mode (unchanged) ────────────────────────────────────
    if (argc == 4) {
        const std::string ticker     = argv[2];
        const std::string output_csv = argv[3];

        std::cout << "LOB Engine — Nasdaq ITCH 5.0 Parser\n"
                  << "  File:    " << itch_file  << "\n"
                  << "  Ticker:  " << ticker     << "\n"
                  << "  Output:  " << output_csv << "\n\n";

        OrderBook      book(ticker);
        FeatureEngine  features;
        size_t msg_count = 0, trade_count = 0;

        ParserCallbacks cb;

        cb.on_add = [&](const AddOrderMsg& m) {
            book.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
        };
        cb.on_delete = [&](const DeleteOrderMsg& m) {
            book.delete_order(m.timestamp_ns, m.order_ref);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
        };
        cb.on_replace = [&](const ReplaceOrderMsg& m) {
            book.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                               m.new_shares, m.new_price);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
        };
        cb.on_execute = [&](const ExecuteOrderMsg& m) {
            features.on_trade('B', m.executed_shares, m.timestamp_ns);
            book.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            features.on_book_update(book, m.timestamp_ns);
            ++msg_count;
            ++trade_count;
        };

        auto t0 = std::chrono::steady_clock::now();
        try {
            ItchParser::parse_file(itch_file, cb, ticker);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

        try {
            features.write_csv(output_csv);
        } catch (const std::exception& e) {
            std::cerr << "Error writing CSV: " << e.what() << "\n";
            return 1;
        }

        std::cout << "Done.\n"
                  << "  Messages processed:  " << msg_count   << "\n"
                  << "  Trades:              " << trade_count << "\n"
                  << "  Feature rows:        " << features.rows().size() << "\n"
                  << "  Active orders left:  " << book.num_orders() << "\n"
                  << "  Elapsed:             " << elapsed_s   << "s\n"
                  << "  Throughput:          "
                  << (msg_count / elapsed_s / 1e6) << "M msg/s\n\n"
                  << "Features written to: " << output_csv << "\n";
        return 0;
    }

    // ── Dual-ticker cross-asset mode ──────────────────────────────────────
    const std::string ticker1    = argv[2];
    const std::string ticker2    = argv[3];
    const std::string output_csv = argv[4];

    std::cout << "LOB Engine — Cross-Asset Mode\n"
              << "  File:    " << itch_file  << "\n"
              << "  Ticker1: " << ticker1    << "\n"
              << "  Ticker2: " << ticker2    << "\n"
              << "  Output:  " << output_csv << "\n\n";

    // order_ref → 0 (ticker1) or 1 (ticker2)
    std::unordered_map<uint64_t, uint8_t> order_route;
    order_route.reserve(1 << 20);  // pre-allocate for ~1M live orders

    OrderBook     book1(ticker1), book2(ticker2);
    FeatureEngine feat1, feat2;
    size_t cnt1 = 0, cnt2 = 0, trade_cnt1 = 0, trade_cnt2 = 0;

    ParserCallbacks cb;

    cb.on_add = [&](const AddOrderMsg& m) {
        std::string stk(m.stock);
        if (stk == ticker1) {
            order_route[m.order_ref] = 0;
            book1.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else if (stk == ticker2) {
            order_route[m.order_ref] = 1;
            book2.add_order(m.timestamp_ns, m.order_ref, m.side, m.shares, m.price);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_delete = [&](const DeleteOrderMsg& m) {
        auto it = order_route.find(m.order_ref);
        if (it == order_route.end()) return;
        uint8_t idx = it->second;
        order_route.erase(it);
        if (idx == 0) {
            book1.delete_order(m.timestamp_ns, m.order_ref);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.delete_order(m.timestamp_ns, m.order_ref);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_replace = [&](const ReplaceOrderMsg& m) {
        auto it = order_route.find(m.old_order_ref);
        if (it == order_route.end()) return;
        uint8_t idx = it->second;
        order_route.erase(it);
        order_route[m.new_order_ref] = idx;
        if (idx == 0) {
            book1.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                                m.new_shares, m.new_price);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1;
        } else {
            book2.replace_order(m.timestamp_ns, m.old_order_ref, m.new_order_ref,
                                m.new_shares, m.new_price);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2;
        }
    };

    cb.on_execute = [&](const ExecuteOrderMsg& m) {
        auto it = order_route.find(m.order_ref);
        if (it == order_route.end()) return;
        uint8_t idx = it->second;
        if (idx == 0) {
            feat1.on_trade('B', m.executed_shares, m.timestamp_ns);
            book1.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            feat1.on_book_update(book1, m.timestamp_ns);
            ++cnt1; ++trade_cnt1;
        } else {
            feat2.on_trade('B', m.executed_shares, m.timestamp_ns);
            book2.execute_order(m.timestamp_ns, m.order_ref, m.executed_shares);
            feat2.on_book_update(book2, m.timestamp_ns);
            ++cnt2; ++trade_cnt2;
        }
    };

    // ── Parse ─────────────────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();
    try {
        ItchParser::parse_file(itch_file, cb);  // no filter — routing done above
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    // ── Guard: both tickers must have produced rows ────────────────────────
    if (feat1.rows().empty()) {
        std::cerr << "Error: " << ticker1 << " produced no feature rows.\n"
                  << "  Verify it appears in the ITCH file.\n";
        return 1;
    }
    if (feat2.rows().empty()) {
        std::cerr << "Error: " << ticker2 << " produced no feature rows.\n"
                  << "  Verify it appears in the ITCH file.\n";
        return 1;
    }

    // ── Merge rows by second-floor bucket ─────────────────────────────────
    std::unordered_map<uint64_t, const FeatureRow*> by_sec1, by_sec2;
    by_sec1.reserve(feat1.rows().size());
    by_sec2.reserve(feat2.rows().size());

    for (const auto& r : feat1.rows())
        by_sec1[r.timestamp_ns / 1'000'000'000ULL] = &r;
    for (const auto& r : feat2.rows())
        by_sec2[r.timestamp_ns / 1'000'000'000ULL] = &r;

    std::vector<uint64_t> common_secs;
    common_secs.reserve(std::min(feat1.rows().size(), feat2.rows().size()));
    for (const auto& [sec, unused] : by_sec1)
        if (by_sec2.count(sec)) common_secs.push_back(sec);
    std::sort(common_secs.begin(), common_secs.end());

    // Column prefixes: lowercase ticker names
    std::string p1 = ticker1, p2 = ticker2;
    std::transform(p1.begin(), p1.end(), p1.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::transform(p2.begin(), p2.end(), p2.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::ofstream csv_out(output_csv);
    if (!csv_out) {
        std::cerr << "Error: cannot write " << output_csv << "\n";
        return 1;
    }
    csv_out << "ts," << p1 << "_mid," << p1 << "_ofi,"
            << p2 << "_mid," << p2 << "_ofi\n";

    for (uint64_t sec : common_secs) {
        const FeatureRow& r1 = *by_sec1.at(sec);
        const FeatureRow& r2 = *by_sec2.at(sec);
        csv_out << r1.timestamp_ns << ","
                << r1.mid_price   << "," << r1.ofi << ","
                << r2.mid_price   << "," << r2.ofi << "\n";
    }

    std::cout << "Done.\n"
              << "  " << ticker1 << " messages:    " << cnt1        << "\n"
              << "  " << ticker2 << " messages:    " << cnt2        << "\n"
              << "  " << ticker1 << " feature rows: " << feat1.rows().size() << "\n"
              << "  " << ticker2 << " feature rows: " << feat2.rows().size() << "\n"
              << "  Combined rows (aligned seconds): " << common_secs.size() << "\n"
              << "  Elapsed: " << elapsed_s << "s\n"
              << "  Throughput: "
              << ((cnt1 + cnt2) / elapsed_s / 1e6) << "M msg/s\n\n"
              << "Combined CSV written to: " << output_csv << "\n";
    return 0;
}
```

- [ ] **Step 3: Build and verify it compiles**

```bash
make clean && make
```

Expected: `Build successful! Binary: ./lob_engine` — no warnings.

- [ ] **Step 4: Run dual-ticker mode on real ITCH data**

```bash
./lob_engine data/01302020.NASDAQ_ITCH50 AAPL SPY data/cross_AAPL_SPY.csv
```

Expected output (approximate):
```
LOB Engine — Cross-Asset Mode
  File:    data/01302020.NASDAQ_ITCH50
  Ticker1: AAPL
  Ticker2: SPY
  Output:  data/cross_AAPL_SPY.csv

Done.
  AAPL messages:    ...
  SPY messages:     ...
  AAPL feature rows: ~55000
  SPY feature rows:  ~55000
  Combined rows (aligned seconds): ~55000
  Elapsed: ...s
  Throughput: ...M msg/s

Combined CSV written to: data/cross_AAPL_SPY.csv
```

If SPY produces 0 rows, it is not in the ITCH file — the engine will exit with an error and instructions.

- [ ] **Step 5: Spot-check the combined CSV**

```bash
head -3 data/cross_AAPL_SPY.csv
wc -l data/cross_AAPL_SPY.csv
```

Expected: header `ts,aapl_mid,aapl_ofi,spy_mid,spy_ofi` followed by data rows; line count ~55000.

- [ ] **Step 6: Verify single-ticker mode still works**

```bash
./lob_engine data/01302020.NASDAQ_ITCH50 AAPL data/features_AAPL.csv
```

Expected: identical output to before this change.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add dual-ticker cross-asset mode to lob_engine"
```

---

## Task 2: `cross_asset_ofi()` in `analysis.py`

**Files:**
- Modify: `scripts/analysis.py`

Five models are run in order. Model 1 (AAPL OFI → AAPL returns) is the baseline; all subsequent models print a ΔR²\_out relative to it.

- [ ] **Step 1: Add `cross_asset_ofi()` function to `analysis.py`**

Insert this function after `depth_informativeness()` and before `main()`:

```python
# ── Research Question 4: Cross-Asset OFI ─────────────────────────────

def cross_asset_ofi(combined_csv: str):
    """
    Q4: Does SPY OFI lead AAPL 1-second returns at lag-1 and lag-2?

    Models:
      1. AAPL OFI (current)              → AAPL fwd_ret_1s   [baseline]
      2. SPY OFI lagged 1s               → AAPL fwd_ret_1s
      3. SPY OFI lagged 2s               → AAPL fwd_ret_1s
      4. AAPL OFI + SPY lag-1            → AAPL fwd_ret_1s
      5. AAPL OFI + SPY lag-1 + SPY lag-2 → AAPL fwd_ret_1s
    """
    df = pd.read_csv(combined_csv)
    df['ts'] = pd.to_datetime(df['ts'], unit='ns')
    df.set_index('ts', inplace=True)

    # Infer ticker names from column names: first two cols are {t1}_mid, {t1}_ofi
    cols = list(df.columns)  # [t1_mid, t1_ofi, t2_mid, t2_ofi]
    t1_mid, t1_ofi, t2_mid, t2_ofi = cols[0], cols[1], cols[2], cols[3]
    t1 = t1_mid.replace('_mid', '').upper()
    t2 = t2_mid.replace('_mid', '').upper()

    # Forward return: what we're predicting (ticker1, 1-second horizon)
    df['t1_fwd_ret'] = df[t1_mid].pct_change().shift(-1)

    # Lagged predictors
    df['t2_ofi_lag1'] = df[t2_ofi].shift(1)
    df['t2_ofi_lag2'] = df[t2_ofi].shift(2)

    # Mask: require all features and target to be non-null
    mask = df[['t1_fwd_ret', t1_ofi, 't2_ofi_lag1', 't2_ofi_lag2']].notna().all(axis=1)
    y    = df.loc[mask, 't1_fwd_ret'].values
    n    = mask.sum()

    print(f"\n=== Q4: Cross-Asset OFI ({t2} OFI → {t1} 1s Returns) ===")
    print(f"  N={n}  N_train={int(n*0.7)}  N_test={n - int(n*0.7)}\n")

    models = [
        (f"{t1} OFI (baseline)",
         df.loc[mask, t1_ofi].values.reshape(-1, 1)),
        (f"{t2} OFI lag-1",
         df.loc[mask, 't2_ofi_lag1'].values.reshape(-1, 1)),
        (f"{t2} OFI lag-2",
         df.loc[mask, 't2_ofi_lag2'].values.reshape(-1, 1)),
        (f"{t1} OFI + {t2} lag-1",
         np.column_stack([df.loc[mask, t1_ofi].values,
                          df.loc[mask, 't2_ofi_lag1'].values])),
        (f"{t1} OFI + {t2} lag-1 + lag-2",
         np.column_stack([df.loc[mask, t1_ofi].values,
                          df.loc[mask, 't2_ofi_lag1'].values,
                          df.loc[mask, 't2_ofi_lag2'].values])),
    ]

    baseline_r2_out = None
    for name, X in models:
        m, r2_in, r2_out = _run_regression(X, y)
        if baseline_r2_out is None:
            delta_str = ""
            baseline_r2_out = r2_out
        else:
            delta = r2_out - baseline_r2_out
            delta_str = f"  Δ={delta:+.4f} vs baseline"
        print(f"  {name:<40}  R²_in={r2_in:.4f}  R²_out={r2_out:.4f}{delta_str}")
```

- [ ] **Step 2: Add `import numpy as np` if not already present**

Check the top of `analysis.py`. It should already have `import numpy as np`. If not, add it after `import pandas as pd`.

- [ ] **Step 3: Update `main()` to accept `--cross` flag**

In `main()`, add after the existing `depth_dir` block:

```python
cross_csv = None
if '--cross' in sys.argv:
    idx = sys.argv.index('--cross')
    if idx + 1 >= len(sys.argv):
        print("Error: --cross requires a path argument")
        sys.exit(1)
    cross_csv = sys.argv[idx + 1]
```

And add at the end of `main()`, after the Q3 block:

```python
if cross_csv:
    cross_asset_ofi(cross_csv)
```

Also update the usage string at the top of `main()`:

```python
print("Usage: python analysis.py <features.csv> [--depth-dir <dir>] [--cross <combined.csv>] [--decay <amzn_features.csv>]")
```

- [ ] **Step 4: Run Q4 on real data**

```bash
python3 scripts/analysis.py data/features_AAPL.csv --cross data/cross_AAPL_SPY.csv
```

Expected: prints Q4 table with 5 models, R²\_in and R²\_out for each, ΔR²\_out for models 2–5.

- [ ] **Step 5: Commit**

```bash
git add scripts/analysis.py
git commit -m "feat: add Q4 cross-asset OFI regression (SPY -> AAPL, lag 1 and 2)"
```

---

## Task 3: `signal_decay()` in `analysis.py`

**Files:**
- Modify: `scripts/analysis.py`

- [ ] **Step 1: Add `signal_decay()` function to `analysis.py`**

Insert this function after `cross_asset_ofi()` and before `main()`:

```python
# ── Addition 2: Multi-Frequency Signal Decay ──────────────────────────

def signal_decay(ticker1_csv: str, ticker2_csv: str):
    """
    Resample 1s feature CSVs to 1/5/10/30/60s, run OFI→fwd_ret regression
    at each horizon, and save results/signal_decay.png.
    """
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from pathlib import Path

    horizons = [1, 5, 10, 30, 60]
    ticker_paths = [(ticker1_csv, None), (ticker2_csv, None)]

    # Infer ticker labels from CSV content
    def ticker_label(path):
        df = pd.read_csv(path, nrows=1)
        # Use filename as fallback label
        return Path(path).stem.replace('features_', '').upper()

    results = {}

    print("\n=== Addition 2: OFI Signal Decay by Aggregation Horizon ===")

    for path in [ticker1_csv, ticker2_csv]:
        label = ticker_label(path)
        df = load_features(path)
        results[label] = {}
        print(f"\n  {label}:")

        for h in horizons:
            if h == 1:
                agg = df[['ofi', 'mid_price']].copy()
            else:
                agg = df[['ofi', 'mid_price']].resample(f'{h}s').agg(
                    {'ofi': 'sum', 'mid_price': 'last'}
                ).dropna()

            agg = agg.copy()
            agg['fwd_ret'] = agg['mid_price'].pct_change().shift(-1)
            mask = agg[['ofi', 'fwd_ret']].notna().all(axis=1)

            if mask.sum() < 50:
                print(f"    {h:2d}s: insufficient data ({mask.sum()} obs) — skipped")
                continue

            X = agg.loc[mask, 'ofi'].values.reshape(-1, 1)
            y = agg.loc[mask, 'fwd_ret'].values
            _, _, r2_out = _run_regression(X, y)
            results[label][h] = r2_out
            print(f"    {h:2d}s  N={mask.sum():5d}  R²_out={r2_out:.4f}")

    # ── Plot ──────────────────────────────────────────────────────────
    Path('results').mkdir(exist_ok=True)

    fig, ax = plt.subplots(figsize=(8, 5))
    colors = {'AAPL': 'steelblue', 'AMZN': 'darkorange'}

    for label, res in results.items():
        if not res:
            continue
        hs  = sorted(res.keys())
        r2s = [res[h] for h in hs]
        color = colors.get(label, 'gray')
        ax.plot(hs, r2s, marker='o', color=color, label=label, linewidth=2, markersize=6)

    ax.axhline(0, color='black', linewidth=0.8, linestyle='--', alpha=0.5, label='R²=0')
    ax.set_xscale('log')
    ax.set_xticks(horizons)
    ax.set_xticklabels([f'{h}s' for h in horizons])
    ax.set_xlabel('Aggregation horizon (log scale)')
    ax.set_ylabel('Out-of-sample R²')
    ax.set_title('OFI Predictive Power vs. Aggregation Horizon')
    ax.legend()
    fig.tight_layout()
    fig.savefig('results/signal_decay.png', dpi=150)
    plt.close(fig)
    print("\n  Saved: results/signal_decay.png")
```

- [ ] **Step 2: Update `main()` to accept `--decay` flag**

In `main()`, add after the `--cross` block:

```python
decay_csv = None
if '--decay' in sys.argv:
    idx = sys.argv.index('--decay')
    if idx + 1 >= len(sys.argv):
        print("Error: --decay requires a path argument")
        sys.exit(1)
    decay_csv = sys.argv[idx + 1]
```

And add at the end of `main()`, after the `cross_csv` block:

```python
if decay_csv:
    signal_decay(feature_file, decay_csv)
```

- [ ] **Step 3: Run signal decay on real data**

```bash
python3 scripts/analysis.py data/features_AAPL.csv --decay data/features_AMZN.csv
```

Expected: prints a table of R²\_out at 1s/5s/10s/30s/60s for both tickers, saves `results/signal_decay.png`.

- [ ] **Step 4: Verify the PNG exists and has two labeled lines**

```bash
ls -lh results/signal_decay.png
```

Expected: file exists, size > 20KB.

- [ ] **Step 5: Commit**

```bash
git add scripts/analysis.py results/signal_decay.png
git commit -m "feat: add signal decay analysis and PNG plot"
```

---

## Task 4: Full-Suite Run, README Update, Push

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Run the full analysis suite end-to-end**

```bash
python3 scripts/analysis.py data/features_AAPL.csv \
    --cross data/cross_AAPL_SPY.csv \
    --decay data/features_AMZN.csv
```

Record all printed R² values — these go into the README.

- [ ] **Step 2: Update the Research Results section in `README.md`**

Add two new subsections after Iteration 3:

**Cross-Asset OFI section** (use actual numbers from Step 1):
```markdown
### Research Question 4 — Cross-Asset OFI (SPY → AAPL)

| Model | Predictor(s) | R²\_in | R²\_out | ΔR²\_out |
|---|---|---|---|---|
| 1 | AAPL OFI (baseline) | ... | ... | — |
| 2 | SPY OFI lag-1 | ... | ... | ... |
| 3 | SPY OFI lag-2 | ... | ... | ... |
| 4 | AAPL OFI + SPY lag-1 | ... | ... | ... |
| 5 | AAPL OFI + SPY lag-1 + lag-2 | ... | ... | ... |

[Interpretation based on actual numbers]
```

**Signal Decay section** (use actual numbers from Step 1):
```markdown
### Addition 2 — Multi-Frequency Signal Decay

| Horizon | AAPL R²\_out | AMZN R²\_out |
|---|---|---|
| 1s | ... | ... |
| 5s | ... | ... |
| 10s | ... | ... |
| 30s | ... | ... |
| 60s | ... | ... |

![Signal Decay](results/signal_decay.png)

[Interpretation based on actual numbers]
```

- [ ] **Step 3: Commit README and push everything**

```bash
git add README.md
git commit -m "docs: add Q4 cross-asset OFI and signal decay results to README"
git push
```

---

## Self-Review Checklist

- **Spec coverage:** dual-ticker single-pass ✓ | combined CSV with 5 columns ✓ | Q4 with 5 models including lag-1 and lag-2 ✓ | signal decay at 5 horizons ✓ | PNG output ✓ | AAPL+AMZN overlay ✓ | `--cross` / `--decay` CLI flags ✓
- **No placeholders:** all code blocks are complete and runnable
- **Type consistency:** `_run_regression(X, y)` is called identically across Q1, Q4, and signal decay (same signature throughout analysis.py); `FeatureRow` fields `timestamp_ns`, `mid_price`, `ofi` used correctly per the existing struct definition
- **Edge cases covered:** empty ticker rows → error + exit before CSV write; missing CLI arg → error message; `mask.sum() < 50` guard in signal_decay
