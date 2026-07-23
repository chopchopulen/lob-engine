"""
panel_analysis.py — multi-day panel OFI regression study.

For each ticker (AAPL, AMZN), for each day in the panel:
  - Regress OFI → 1-second forward mid-price return (70/30 time-series split)
  - Record OOS R²

Then:
  - Plot R² by date for both tickers
  - T-test: is AMZN R² systematically higher than AAPL?
  - Print mean R² ± std per ticker and per market condition

Reads:  data/panel_AAPL.csv
        data/panel_AMZN.csv
Writes: results/panel_r2_by_date.png
        results/panel_summary.csv

Usage:
    python scripts/panel_analysis.py [--data-dir data/] [--results-dir results/]
"""

import sys
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import stats
from sklearn.linear_model import LinearRegression


# ── Regression helper ────────────────────────────────────────────────────────

def oos_r2(ofi: np.ndarray, fwd_ret: np.ndarray) -> float | None:
    """70/30 time-series OLS. Returns OOS R² or None if insufficient data."""
    n = len(ofi)
    if n < 50:
        return None
    split = int(n * 0.7)
    X = ofi.reshape(-1, 1)
    m = LinearRegression()
    m.fit(X[:split], fwd_ret[:split])
    return float(m.score(X[split:], fwd_ret[split:]))


# ── Per-day regression ───────────────────────────────────────────────────────

def run_daily_regressions(panel: pd.DataFrame, ticker: str) -> pd.DataFrame:
    """
    For each date in panel, compute OFI → fwd_ret_1s OOS R².
    Returns DataFrame with columns: date, market_condition, r2_oos, n_obs.
    """
    records = []

    for date, group in panel.groupby("date"):
        g = group.copy().sort_values("ts").reset_index(drop=True)
        condition = g["market_condition"].iloc[0]

        # 1-second forward return: shift mid_price by 1 row (rows are already ~1s)
        g["fwd_ret"] = g["mid_price"].pct_change().shift(-1)
        mask = g["ofi"].notna() & g["fwd_ret"].notna()
        g = g[mask]

        r2 = oos_r2(g["ofi"].values, g["fwd_ret"].values)

        records.append(
            {
                "ticker": ticker,
                "date": date,
                "market_condition": condition,
                "r2_oos": r2,
                "n_obs": len(g),
            }
        )

    df = pd.DataFrame(records)
    df["date"] = pd.to_datetime(df["date"])
    df.sort_values("date", inplace=True)
    df.reset_index(drop=True, inplace=True)
    return df


# ── Summary stats ────────────────────────────────────────────────────────────

def print_summary(results: pd.DataFrame) -> None:
    valid = results.dropna(subset=["r2_oos"])

    print("\n── Mean OOS R² by ticker ───────────────────────────────────")
    for ticker, grp in valid.groupby("ticker"):
        r2s = grp["r2_oos"].values
        print(f"  {ticker:<6}  mean={r2s.mean():.4f}  std={r2s.std():.4f}"
              f"  median={np.median(r2s):.4f}  n={len(r2s)}")

    print("\n── Mean OOS R² by market condition ─────────────────────────")
    for (ticker, cond), grp in valid.groupby(["ticker", "market_condition"]):
        r2s = grp["r2_oos"].values
        print(f"  {ticker:<6}  {cond:<10}  mean={r2s.mean():.4f}  n={len(r2s)}")

    # ── T-test: AMZN > AAPL? ────────────────────────────────────────────────
    print("\n── T-test: AMZN R² > AAPL R²? ─────────────────────────────")
    aapl = valid[valid["ticker"] == "AAPL"]["r2_oos"].values
    amzn = valid[valid["ticker"] == "AMZN"]["r2_oos"].values

    if len(aapl) < 2 or len(amzn) < 2:
        print("  Insufficient data for t-test")
        return

    # Independent samples t-test (one-sided: AMZN > AAPL)
    t_stat, p_two = stats.ttest_ind(amzn, aapl, equal_var=False)
    p_one = p_two / 2 if t_stat > 0 else 1 - p_two / 2

    print(f"  AAPL  mean R²={aapl.mean():.4f}  n={len(aapl)}")
    print(f"  AMZN  mean R²={amzn.mean():.4f}  n={len(amzn)}")
    print(f"  t={t_stat:.3f}  p(one-sided)={p_one:.4f}", end="")
    if p_one < 0.05:
        print("  → AMZN R² significantly higher (p<0.05)")
    elif p_one < 0.10:
        print("  → marginal (p<0.10)")
    else:
        print("  → not significant")

    # Paired t-test on matched dates
    merged = pd.merge(
        valid[valid["ticker"] == "AAPL"][["date", "r2_oos"]].rename(columns={"r2_oos": "aapl"}),
        valid[valid["ticker"] == "AMZN"][["date", "r2_oos"]].rename(columns={"r2_oos": "amzn"}),
        on="date",
    )
    if len(merged) >= 2:
        t2, p2 = stats.ttest_rel(merged["amzn"].values, merged["aapl"].values)
        p2_one = p2 / 2 if t2 > 0 else 1 - p2 / 2
        print(f"  Paired (matched dates, n={len(merged)}): "
              f"t={t2:.3f}  p(one-sided)={p2_one:.4f}", end="")
        if p2_one < 0.05:
            print("  → significant")
        else:
            print("  → not significant")


# ── Plot ─────────────────────────────────────────────────────────────────────

def plot_r2_by_date(results: pd.DataFrame, out_path: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates

    fig, ax = plt.subplots(figsize=(12, 5))

    colors = {"AAPL": "steelblue", "AMZN": "darkorange"}
    markers = {"AAPL": "o", "AMZN": "s"}

    condition_colors = {
        "normal": "white",
        "high_vol": "#ffcccc",
        "low_vol": "#ccffcc",
        "earnings": "#ccccff",
        "unknown": "#eeeeee",
    }

    valid = results.dropna(subset=["r2_oos"])

    # Shade background by market condition (use AAPL dates as reference)
    aapl_days = valid[valid["ticker"] == "AAPL"].sort_values("date")
    for _, row in aapl_days.iterrows():
        ax.axvspan(
            row["date"] - pd.Timedelta(days=0.4),
            row["date"] + pd.Timedelta(days=0.4),
            color=condition_colors.get(row["market_condition"], "#eeeeee"),
            alpha=0.3,
            zorder=0,
        )

    for ticker, grp in valid.groupby("ticker"):
        grp = grp.sort_values("date")
        ax.plot(
            grp["date"],
            grp["r2_oos"],
            marker=markers.get(ticker, "o"),
            color=colors.get(ticker, "gray"),
            label=ticker,
            linewidth=1.5,
            markersize=7,
        )

    ax.axhline(0, color="black", linewidth=0.8, linestyle="--", alpha=0.5)

    # Legend for market conditions
    from matplotlib.patches import Patch
    condition_labels = {
        "normal": "Normal",
        "high_vol": "High-vol (COVID)",
        "low_vol": "Low-vol",
        "earnings": "AAPL earnings",
    }
    legend_patches = [
        Patch(facecolor=condition_colors[k], edgecolor="gray", alpha=0.5, label=v)
        for k, v in condition_labels.items()
        if k in valid["market_condition"].values
    ]

    ticker_legend = ax.legend(loc="upper left", framealpha=0.9)
    ax.add_artist(ticker_legend)
    ax.legend(handles=legend_patches, loc="upper right", framealpha=0.9, fontsize=8)

    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d"))
    fig.autofmt_xdate(rotation=35)
    ax.set_ylabel("Out-of-sample R² (OFI → 1s return)")
    ax.set_title("OFI Predictive Power by Date: AAPL vs AMZN")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"\n  Saved: {out_path}")


# ── Multi-horizon signal decay (panel) ───────────────────────────────────────

HORIZONS: list[tuple[str, float]] = [
    ("500ms", 0.5),
    ("1s",    1.0),
    ("5s",    5.0),
    ("10s",  10.0),
    ("30s",  30.0),
    ("60s",  60.0),
]


def signal_decay_panel(data_dir: Path, results_dir: Path, tickers: list[str]) -> None:
    """
    For each ticker and each panel day, resample to multiple horizons and run
    OFI → 1-period-ahead return regression. Plot mean OOS R² ± std across days.

    Writes: results/signal_decay_panel.png
    """
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # results[ticker][horizon_sec] = list of per-day OOS R² values
    decay: dict[str, dict[float, list[float]]] = {}
    n_days: dict[str, int] = {}

    for ticker in tickers:
        panel_path = data_dir / f"panel_{ticker}.csv"
        if not panel_path.exists():
            print(f"  WARN: {panel_path} not found — skipping {ticker}", file=sys.stderr)
            continue

        panel = pd.read_csv(panel_path)
        panel["ts"] = pd.to_datetime(panel["ts"], unit="ns")
        panel.set_index("ts", inplace=True)
        panel.sort_index(inplace=True)

        decay[ticker] = {h: [] for _, h in HORIZONS}
        day_count = 0

        for date, group in panel.groupby("date"):
            g = group[["ofi", "mid_price"]].copy()

            for freq_str, horizon_sec in HORIZONS:
                agg = g.resample(freq_str).agg({"ofi": "sum", "mid_price": "last"}).dropna()
                agg = agg.copy()
                agg["fwd_ret"] = agg["mid_price"].pct_change().shift(-1)
                valid = agg[["ofi", "fwd_ret"]].dropna()

                r2 = oos_r2(valid["ofi"].values, valid["fwd_ret"].values)
                if r2 is not None:
                    decay[ticker][horizon_sec].append(r2)

            day_count += 1

        n_days[ticker] = day_count

    if not decay:
        print("  No data for signal decay panel — check panel CSVs.", file=sys.stderr)
        return

    # ── Print table ──────────────────────────────────────────────────────────
    print("\n=== Multi-Horizon Signal Decay (Panel) ===")
    header = f"  {'Horizon':<8}"
    for ticker in decay:
        header += f"  {ticker} mean R²  {ticker} std   n_days"
    print(header)

    for freq_str, horizon_sec in HORIZONS:
        row = f"  {freq_str:<8}"
        for ticker, by_horizon in decay.items():
            vals = by_horizon[horizon_sec]
            if vals:
                row += f"  {np.mean(vals):+.4f}       {np.std(vals):.4f}    {len(vals)}"
            else:
                row += f"  {'N/A':<14}  {'':6}  {0}"
        print(row)

    # ── Plot ─────────────────────────────────────────────────────────────────
    colors  = {"AAPL": "steelblue", "AMZN": "darkorange",
               "ETSY": "#2ca02c",  "NFLX": "#d62728", "WDAY": "#9467bd"}
    markers = {"AAPL": "o", "AMZN": "s", "ETSY": "^", "NFLX": "D", "WDAY": "v"}

    fig, ax = plt.subplots(figsize=(9, 5))

    horizon_secs = [h for _, h in HORIZONS]

    for ticker, by_horizon in decay.items():
        means, stds, xs = [], [], []
        for horizon_sec in horizon_secs:
            vals = by_horizon[horizon_sec]
            if vals:
                means.append(np.mean(vals))
                stds.append(np.std(vals))
                xs.append(horizon_sec)

        if not xs:
            continue

        nd = n_days.get(ticker, "?")
        ax.errorbar(
            xs, means,
            yerr=stds,
            marker=markers.get(ticker, "o"),
            color=colors.get(ticker, "gray"),
            label=f"{ticker} (n={nd} days)",
            linewidth=2,
            markersize=7,
            capsize=4,
            capthick=1.5,
        )

    ax.axhline(0, color="black", linewidth=0.8, linestyle="--", alpha=0.5, label="R²=0")
    ax.set_xscale("log")
    ax.set_xticks(horizon_secs)
    ax.set_xticklabels([f for f, _ in HORIZONS])
    ax.set_xlabel("Aggregation horizon (log scale)")
    ax.set_ylabel("Mean out-of-sample R² across panel days")
    ax.set_title("OFI Signal Decay by Horizon — Panel (All Tickers)")
    ax.legend(framealpha=0.9)
    fig.tight_layout()

    out_path = results_dir / "signal_decay_panel.png"
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"\n  Saved: {out_path}")


# ── Cross-ticker comparison ───────────────────────────────────────────────────

def print_cross_ticker_comparison(
    results: pd.DataFrame, panel_rows: dict[str, tuple[int, int]]
) -> None:
    """
    Print cross-ticker table: ticker, mean OOS R², std, n_days, avg rows/day.
    panel_rows: {ticker: (total_rows, n_days)}
    Sorted by avg rows/day descending (liquidity proxy).
    """
    valid = results.dropna(subset=["r2_oos"])
    rows = []
    for ticker, grp in valid.groupby("ticker"):
        r2s = grp["r2_oos"].values
        total, ndays = panel_rows.get(ticker, (0, 1))
        avg_rows = total / ndays if ndays else 0
        rows.append(
            {
                "ticker": ticker,
                "mean_r2": r2s.mean(),
                "std_r2": r2s.std(),
                "n_days": len(r2s),
                "avg_rows_day": avg_rows,
            }
        )

    rows.sort(key=lambda x: x["avg_rows_day"], reverse=True)

    print("\n=== Cross-Ticker OFI Predictability ===")
    print(f"  {'Ticker':<6}  {'Mean R²':>9}  {'Std':>7}  {'n_days':>6}  "
          f"{'Avg rows/day':>14}  (liquidity proxy)")
    print("  " + "─" * 60)
    for r in rows:
        print(f"  {r['ticker']:<6}  {r['mean_r2']:>+9.4f}  {r['std_r2']:>7.4f}  "
              f"{r['n_days']:>6}  {r['avg_rows_day']:>14,.0f}")

    if len(rows) >= 2:
        # Spearman rank: does lower liquidity (fewer rows) → higher R²?
        from scipy.stats import spearmanr
        liquidity = [r["avg_rows_day"] for r in rows]
        mean_r2   = [r["mean_r2"] for r in rows]
        rho, pval = spearmanr(liquidity, mean_r2)
        print(f"\n  Spearman ρ(liquidity, mean R²) = {rho:.3f}  p={pval:.3f}", end="")
        if rho < -0.3:
            print("  → less-liquid tickers show higher R² (hypothesis supported)")
        elif rho > 0.3:
            print("  → more-liquid tickers show higher R² (hypothesis rejected)")
        else:
            print("  → no clear monotone relationship")


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    data_dir = Path("data")
    results_dir = Path("results")

    args = sys.argv[1:]
    for i, arg in enumerate(args):
        if arg == "--data-dir" and i + 1 < len(args):
            data_dir = Path(args[i + 1])
        if arg == "--results-dir" and i + 1 < len(args):
            results_dir = Path(args[i + 1])

    results_dir.mkdir(exist_ok=True)

    # Auto-discover tickers from panel CSVs
    tickers = sorted(
        p.stem.replace("panel_", "")
        for p in data_dir.glob("panel_*.csv")
    )
    if not tickers:
        print("No panel_*.csv files found — run build_panel.py first", file=sys.stderr)
        sys.exit(1)
    print(f"Tickers found: {tickers}")

    all_results = []
    panel_rows: dict[str, tuple[int, int]] = {}  # ticker → (total_rows, n_days)

    for ticker in tickers:
        panel_path = data_dir / f"panel_{ticker}.csv"
        if not panel_path.exists():
            print(f"WARN: {panel_path} not found — run build_panel.py first", file=sys.stderr)
            continue

        print(f"\nLoading {panel_path}...")
        panel = pd.read_csv(panel_path)
        n_days = panel["date"].nunique()
        print(f"  {len(panel)} rows, {n_days} days")
        panel_rows[ticker] = (len(panel), n_days)

        print(f"Running daily regressions for {ticker}...")
        daily = run_daily_regressions(panel, ticker)

        for _, row in daily.iterrows():
            r2_str = f"{row['r2_oos']:.4f}" if row["r2_oos"] is not None else "N/A"
            print(f"  {row['date'].date()}  {row['market_condition']:<10}  "
                  f"R²={r2_str}  n={row['n_obs']}")

        all_results.append(daily)

    if not all_results:
        print("No results — check that panel CSVs exist.", file=sys.stderr)
        sys.exit(1)

    results = pd.concat(all_results, ignore_index=True)

    summary_path = results_dir / "panel_summary.csv"
    results.to_csv(summary_path, index=False)
    print(f"\n  Saved: {summary_path}")

    print_summary(results)

    plot_r2_by_date(results, results_dir / "panel_r2_by_date.png")

    signal_decay_panel(data_dir, results_dir, tickers)

    print_cross_ticker_comparison(results, panel_rows)

    print("\nDone.")


if __name__ == "__main__":
    main()
