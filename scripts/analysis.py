"""
Empirical microstructure analysis — Layer 4.
Run after exporting features from the C++ engine.

Three research questions:
1. Does OFI predict short-term returns? (Cont et al. 2014)
2. How does bid-ask spread respond to volatility regimes?
3. How informative are deeper book levels?

Usage:
    python scripts/analysis.py data/features.csv
"""

import sys
import numpy as np
import pandas as pd
from pathlib import Path

# ── Data Loading ─────────────────────────────────────────────────────

def load_features(path: str) -> pd.DataFrame:
    """Load feature CSV exported by the C++ engine."""
    df = pd.read_csv(path)
    df['ts'] = pd.to_datetime(df['ts'], unit='ns')
    df.set_index('ts', inplace=True)
    return df


# ── Research Question 1: OFI → Returns ──────────────────────────────

def _run_regression(X, y):
    """Run time-series 70/30 split regression, return (model, r2_in, r2_out)."""
    from sklearn.linear_model import LinearRegression
    split = int(len(y) * 0.7)
    model = LinearRegression()
    model.fit(X[:split], y[:split])
    return model, model.score(X[:split], y[:split]), model.score(X[split:], y[split:])


def ofi_return_regression(df: pd.DataFrame, horizons_sec: list[float] = [1.0, 10.0]):
    """
    Regress future mid-price returns on raw OFI AND normalized OFI.
    Reproduces Cont, Kukanov & Stoikov (2014), Section 4, and compares
    whether normalization by avg trade size improves the R².

    Uses time-series train/test splits (NOT random).
    """
    has_norm = 'normalized_ofi' in df.columns
    results = {}

    for h in horizons_sec:
        shift_periods = max(1, int(h / df.index.to_series().diff().dt.total_seconds().median()))
        fwd_col = f'fwd_ret_{h}s'
        df[fwd_col] = df['mid_price'].pct_change(shift_periods).shift(-shift_periods)

        mask = df[fwd_col].notna() & df['ofi'].notna()
        y = df.loc[mask, fwd_col].values

        if len(y) < 100:
            print(f"  {h}s horizon: insufficient data ({len(y)} obs)")
            continue

        print(f"  {h}s horizon  (N_train={int(len(y)*0.7)}, N_test={len(y)-int(len(y)*0.7)}):")

        # Raw OFI
        X_raw = df.loc[mask, 'ofi'].values.reshape(-1, 1)
        m_raw, r2_in_raw, r2_out_raw = _run_regression(X_raw, y)
        print(f"    raw OFI        coef={m_raw.coef_[0]:.4e}  R²_in={r2_in_raw:.4f}  R²_out={r2_out_raw:.4f}")

        row = {'raw_r2_in': r2_in_raw, 'raw_r2_out': r2_out_raw}

        # Normalized OFI (if column present)
        if has_norm:
            mask_n = mask & df['normalized_ofi'].notna() & (df['normalized_ofi'] != 0)
            y_n = df.loc[mask_n, fwd_col].values
            if len(y_n) >= 100:
                X_norm = df.loc[mask_n, 'normalized_ofi'].values.reshape(-1, 1)
                m_norm, r2_in_norm, r2_out_norm = _run_regression(X_norm, y_n)
                print(f"    norm OFI       coef={m_norm.coef_[0]:.4e}  R²_in={r2_in_norm:.4f}  R²_out={r2_out_norm:.4f}")
                delta = r2_out_norm - r2_out_raw
                sign = "+" if delta >= 0 else ""
                print(f"    improvement    ΔR²_out={sign}{delta:.4f}  ({'better' if delta > 0 else 'worse' if delta < 0 else 'same'})")
                row.update({'norm_r2_in': r2_in_norm, 'norm_r2_out': r2_out_norm})
            else:
                print(f"    norm OFI       insufficient non-zero rows ({len(y_n)})")
        else:
            print("    norm OFI       column not found in CSV — rebuild the engine")

        results[h] = row

    return results


# ── Research Question 2: Spread vs Volatility ────────────────────────

def spread_volatility_analysis(df: pd.DataFrame, vol_window: str = '5min'):
    """
    Segment data by rolling realized variance and show that spreads
    widen in high-volatility regimes (Glosten-Milgrom adverse selection).
    """
    # Rolling 5-min realized variance from mid-price returns
    ret = df['mid_price'].pct_change()
    df['realized_var'] = ret.rolling(vol_window).var()

    mask = df['realized_var'].notna() & df['quoted_spread'].notna()
    data = df.loc[mask]

    if len(data) < 100:
        print("  Insufficient data for volatility analysis")
        return None

    # Segment into quintiles of realized variance
    data = data.copy()
    # Filter out zero-variance rows to avoid duplicate bin edges
    data = data[data['realized_var'] > 0]
    data['vol_quintile'] = pd.qcut(data['realized_var'], 5, labels=['Q1 (low)', 'Q2', 'Q3', 'Q4', 'Q5 (high)'])

    summary = data.groupby('vol_quintile')['quoted_spread'].agg(['mean', 'median', 'std', 'count'])
    print("\n  Quoted Spread by Volatility Quintile:")
    print(summary.to_string())

    # Spearman rank correlation
    from scipy.stats import spearmanr
    rho, pval = spearmanr(data['realized_var'], data['quoted_spread'])
    print(f"\n  Spearman correlation (variance, spread): {rho:.4f} (p = {pval:.2e})")

    return summary


# ── Research Question 3: Depth-of-Book Informativeness ───────────────

def depth_informativeness(feature_dir: str):
    """
    Compare OFI computed from Level-1 vs Level-5 vs Level-10.
    This requires running the C++ engine 3 times with --depth 1/5/10
    and saving outputs to separate files.

    Expected files:
        features_depth1.csv, features_depth5.csv, features_depth10.csv
    """
    from sklearn.linear_model import LinearRegression

    dir_path = Path(feature_dir)
    depths = [1, 5, 10]
    results = {}

    for d in depths:
        fpath = dir_path / f'features_depth{d}.csv'
        if not fpath.exists():
            print(f"  Missing {fpath} — run: ./lob_replay data.dbn --depth {d} --features {fpath}")
            continue

        df = load_features(str(fpath))

        # 1-second forward returns
        shift = max(1, int(1.0 / df.index.to_series().diff().dt.total_seconds().median()))
        df['fwd_ret'] = df['mid_price'].pct_change(shift).shift(-shift)

        mask = df['fwd_ret'].notna() & df['ofi'].notna()
        X = df.loc[mask, 'ofi'].values.reshape(-1, 1)
        y = df.loc[mask, 'fwd_ret'].values

        if len(y) < 100:
            print(f"  Depth {d}: insufficient data")
            continue

        split = int(len(y) * 0.7)
        model = LinearRegression()
        model.fit(X[:split], y[:split])

        r2_out = model.score(X[split:], y[split:])
        results[d] = r2_out
        print(f"  Depth {d:2d} — Out-of-sample R²: {r2_out:.4f} (N={len(y)})")

    if len(results) > 1:
        best = max(results, key=results.get)
        print(f"\n  Best depth level: {best} (R² = {results[best]:.4f})")
        print("  Note: deeper book info is not guaranteed to help — it depends on")
        print("  the instrument's liquidity profile and queue dynamics.")

    return results


# ── Main ─────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python analysis.py <features.csv> [--depth-dir <dir>]")
        sys.exit(1)

    feature_file = sys.argv[1]
    depth_dir = None
    if '--depth-dir' in sys.argv:
        depth_dir = sys.argv[sys.argv.index('--depth-dir') + 1]

    print(f"Loading features from: {feature_file}")
    df = load_features(feature_file)
    print(f"  Rows: {len(df)}, Columns: {list(df.columns)}")
    print(f"  Time range: {df.index[0]} to {df.index[-1]}")

    print("\n=== Q1: OFI → Short-Term Returns ===")
    ofi_return_regression(df)

    print("\n=== Q2: Spread vs Volatility Regimes ===")
    spread_volatility_analysis(df)

    if depth_dir:
        print("\n=== Q3: Depth-of-Book Informativeness ===")
        depth_informativeness(depth_dir)
    else:
        print("\n=== Q3: Depth-of-Book Informativeness ===")
        print("  Skipped — pass --depth-dir <dir> with features_depth{1,5,10}.csv")


if __name__ == '__main__':
    main()
