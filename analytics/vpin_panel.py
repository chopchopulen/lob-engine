"""
vpin_panel.py — panel-wide (7 dates x 5 tickers) VPIN/Lee-Ready analysis.

Reframed per 2026-07-25 direction:
  Task 1: hidden-volume share per ticker, pooled across dates. The spread/
          price relationship is reported as DIRECTIONAL only (n=5, spread and
          price are collinear here -- this is one finding measured two ways,
          not two independent confirmations).
  Task 2: Lee-Ready scored ONLY on 'C' trades (executed-with-price), pooled
          across all 7 dates per ticker. 'E' is EXCLUDED from scoring --
          ground truth is defined by which side of the book was hit, and 'E'
          fills exactly at that level, so scoring quote-rule against ground
          truth on 'E' is circular by construction, not a real test.
  Task 3: primary deliverable is classification DIVERGENCE, not a VPIN time
          series. Per volume bucket (reset within each ticker-date, partial
          tail bucket discarded), compute the buy-fraction 3 ways (ground
          truth on labelable volume only, Lee-Ready, BVC) and report how far
          BVC/Lee-Ready diverge from ground truth. Non-canonical smaller-
          bucket/shorter-window VPIN counts reported separately, explicitly
          flagged as a deviation from canonical 1/50-ADV, N=50.

Reads analytics/trades/trades_<TICKER>_<DATE>.csv for all 7 panel dates.
Offline analytics only.
"""

import math
import json
import numpy as np
import pandas as pd

from vpin_pipeline import (
    lee_ready, REGULAR_SESSION_START_SEC, REGULAR_SESSION_END_SEC,
)

TICKERS = ["AAPL", "AMZN", "ETSY", "NFLX", "WDAY"]
DATES = ["01302019", "03272019", "07302019", "08302019", "10302019", "12302019", "01302020"]
TRADES_DIR = "analytics/trades"
MIN_RELIABLE_C = 100   # below this pooled count, don't report an accuracy number


def load(ticker, date):
    path = f"{TRADES_DIR}/trades_{ticker}_{date}.csv"
    try:
        df = pd.read_csv(path)
    except FileNotFoundError:
        return None
    df["sec_of_day"] = df["ts"] / 1e9
    df["in_session"] = (df["sec_of_day"] >= REGULAR_SESSION_START_SEC) & \
                        (df["sec_of_day"] <= REGULAR_SESSION_END_SEC)
    df["countable"] = df["printable"] == "Y"
    df["price_d"] = df["price"] / 10000.0
    df["bid_d"] = df["prevailing_bid"] / 10000.0
    df["ask_d"] = df["prevailing_ask"] / 10000.0
    df["mid_d"] = (df["bid_d"] + df["ask_d"]) / 2.0
    df["spread_d"] = df["ask_d"] - df["bid_d"]
    df["ticker"] = ticker
    df["date"] = date
    return df


def load_all():
    per_ticker_date = {}
    missing = []
    for t in TICKERS:
        for d in DATES:
            df = load(t, d)
            if df is None:
                missing.append((t, d))
                continue
            per_ticker_date[(t, d)] = df[df["in_session"] & df["countable"]].sort_values("ts").reset_index(drop=True)
    return per_ticker_date, missing


# ── Task 1: hidden-volume share, pooled ───────────────────────────────────────

def task1(per_td):
    rows = []
    for t in TICKERS:
        dates_here = [d for d in DATES if (t, d) in per_td]
        total_vol = hidden_vol = 0
        spreads, prices = [], []
        for d in dates_here:
            df = per_td[(t, d)]
            total_vol += df["shares"].sum()
            hidden_vol += df.loc[df["gt_side"] == "U", "shares"].sum()
            valid_q = df[(df["bid_d"] > 0) & (df["ask_d"] > 0)]
            if len(valid_q):
                spreads.append(valid_q["spread_d"].median())
                prices.append(valid_q["price_d"].median())
        coverage = 1 - hidden_vol / total_vol if total_vol else float("nan")
        rows.append({
            "ticker": t, "n_dates": len(dates_here),
            "total_vol": int(total_vol), "hidden_vol": int(hidden_vol),
            "hidden_pct": 100.0 * hidden_vol / total_vol if total_vol else float("nan"),
            "coverage_pct": 100.0 * coverage,
            "avg_median_spread": float(np.mean(spreads)) if spreads else float("nan"),
            "avg_median_price": float(np.mean(prices)) if prices else float("nan"),
        })
    tbl = pd.DataFrame(rows).sort_values("hidden_pct", ascending=False).reset_index(drop=True)

    # Directional check only -- n=5, spread & price collinear here, so this is
    # ONE finding (hidden share tracks "expensive/wide" names) measured two
    # ways, not two independent confirmations. Report rank order, not a
    # precision-implying r.
    by_hidden = tbl.sort_values("hidden_pct", ascending=False)["ticker"].tolist()
    by_spread = tbl.sort_values("avg_median_spread", ascending=False)["ticker"].tolist()
    by_price = tbl.sort_values("avg_median_price", ascending=False)["ticker"].tolist()
    concordant_spread = by_hidden == by_spread
    concordant_price = by_hidden == by_price
    return tbl, by_hidden, by_spread, by_price, concordant_spread, concordant_price


# ── Task 2: Lee-Ready scored ONLY on 'C', pooled across dates ────────────────

def task2(per_td):
    scored_by_td = {}
    for (t, d), df in per_td.items():
        scored_by_td[(t, d)] = lee_ready(df)

    rows = []
    for t in TICKERS:
        c_frames = []
        for d in DATES:
            if (t, d) not in scored_by_td:
                continue
            sc = scored_by_td[(t, d)]
            c_lit = sc[(sc["event_type"] == "C") & (sc["gt_side"] != "U")]
            c_frames.append(c_lit)
        pooled_c = pd.concat(c_frames) if c_frames else pd.DataFrame()
        n = len(pooled_c)
        if n < MIN_RELIABLE_C:
            rows.append({"ticker": t, "pooled_c_n": n, "reliable": False,
                         "agree_pct": None, "quote_n": None, "quote_agree_pct": None,
                         "tick_n": None, "tick_agree_pct": None,
                         "at_midpoint_n": None, "at_midpoint_pct": None})
            continue
        agree = (pooled_c["lr_side"] == pooled_c["gt_side"]).sum()
        overall = 100.0 * agree / n
        quote = pooled_c[pooled_c["lr_method"] == "quote"]
        tick = pooled_c[pooled_c["lr_method"].isin(["tick", "inherit"])]
        at_mid = pooled_c[pooled_c["price_d"] == pooled_c["mid_d"]]
        rows.append({
            "ticker": t, "pooled_c_n": n, "reliable": True,
            "agree_pct": overall,
            "quote_n": len(quote),
            "quote_agree_pct": 100.0 * (quote["lr_side"] == quote["gt_side"]).sum() / len(quote) if len(quote) else None,
            "tick_n": len(tick),
            "tick_agree_pct": 100.0 * (tick["lr_side"] == tick["gt_side"]).sum() / len(tick) if len(tick) else None,
            "at_midpoint_n": len(at_mid),
            "at_midpoint_pct": 100.0 * len(at_mid) / n,
        })
    return pd.DataFrame(rows), scored_by_td


# ── Task 3: per-bucket 3-way divergence (primary), non-canonical VPIN (secondary) ──

def bucket_series(df, adv_divisor, window):
    """df must already be LR-scored, in-session, countable, sorted by ts,
    for one (ticker, date). Returns bucket-level DataFrame with buy fractions
    3 ways, plus how many complete `window`-bucket VPIN observations result."""
    total_vol = df["shares"].sum()
    if total_vol == 0:
        return pd.DataFrame(), 0
    bucket_size = total_vol / adv_divisor

    buckets = []
    cum = 0
    b_gt_buy = b_gt_sell = 0
    b_lr_buy = b_lr_sell = 0
    b_vol = 0
    b_last_price = None

    for _, row in df.iterrows():
        shares, price = row["shares"], row["price_d"]
        b_last_price = price
        if row["gt_side"] == "B":
            b_gt_buy += shares
        elif row["gt_side"] == "S":
            b_gt_sell += shares
        if row["lr_side"] == "B":
            b_lr_buy += shares
        elif row["lr_side"] == "S":
            b_lr_sell += shares
        b_vol += shares
        cum += shares
        if cum >= bucket_size:
            buckets.append({
                "vol": b_vol, "close_price": b_last_price,
                "gt_buy": b_gt_buy, "gt_sell": b_gt_sell,
                "lr_buy": b_lr_buy, "lr_sell": b_lr_sell,
            })
            cum = 0
            b_gt_buy = b_gt_sell = b_lr_buy = b_lr_sell = b_vol = 0
    if not buckets:
        return pd.DataFrame(), 0

    bt = pd.DataFrame(buckets)
    price_changes = bt["close_price"].diff()
    sigma = price_changes.std(ddof=0)
    if sigma == 0 or math.isnan(sigma):
        z = pd.Series([0.5] * len(bt))
    else:
        z = price_changes.apply(lambda dp: 0.5 if pd.isna(dp) else
                                 0.5 * (1 + math.erf((dp / sigma) / math.sqrt(2))))
    bt["bvc_buy"] = bt["vol"] * z
    bt["bvc_sell"] = bt["vol"] * (1 - z)

    # buy fractions -- GT fraction is over LABELABLE volume only (gt_buy+gt_sell),
    # explicitly excluding hidden 'P'/unlabeled volume from both numerator and
    # denominator. LR/BVC fractions are over ALL bucket volume. This denominator
    # mismatch is a real limitation, stated in the report, not hidden here.
    gt_denom = (bt["gt_buy"] + bt["gt_sell"]).replace(0, np.nan)
    bt["gt_buy_frac"] = bt["gt_buy"] / gt_denom
    bt["lr_buy_frac"] = bt["lr_buy"] / bt["vol"]
    bt["bvc_buy_frac"] = bt["bvc_buy"] / bt["vol"]
    bt["gt_labelable_frac"] = gt_denom / bt["vol"]   # how much of this bucket GT can even see

    # complete-window VPIN count (secondary deliverable)
    imb_gt = (bt["gt_buy"] - bt["gt_sell"]).abs()
    roll_imb = imb_gt.rolling(window).sum()
    roll_vol = bt["vol"].rolling(window).sum()
    n_complete = int((roll_imb / roll_vol).notna().sum())

    return bt, n_complete


def task3(scored_by_td):
    # Primary: canonical 1/50-ADV bucketing, pooled divergence stats.
    all_buckets = []
    for (t, d), df in scored_by_td.items():
        bt, _ = bucket_series(df, adv_divisor=50, window=50)
        if bt.empty:
            continue
        bt = bt.dropna(subset=["gt_buy_frac"]).copy()
        bt["ticker"] = t
        bt["date"] = d
        all_buckets.append(bt)
    pooled = pd.concat(all_buckets, ignore_index=True) if all_buckets else pd.DataFrame()

    if pooled.empty:
        return pd.DataFrame(), pooled, pd.DataFrame()

    pooled["bvc_err"] = (pooled["bvc_buy_frac"] - pooled["gt_buy_frac"]).abs()
    pooled["lr_err"] = (pooled["lr_buy_frac"] - pooled["gt_buy_frac"]).abs()

    per_ticker = pooled.groupby("ticker").agg(
        n_buckets=("bvc_err", "size"),
        bvc_mae=("bvc_err", "mean"),
        lr_mae=("lr_err", "mean"),
        avg_gt_labelable_frac=("gt_labelable_frac", "mean"),
    ).reset_index()

    overall = {
        "n_buckets": len(pooled),
        "bvc_mae": float(pooled["bvc_err"].mean()),
        "lr_mae": float(pooled["lr_err"].mean()),
    }

    # Divergence-vs-conditions: correlate error with (a) how much of the
    # bucket is hidden/unlabelable, (b) bucket volatility (|close-price change|
    # proxy already embedded via price_changes -- reuse bucket-to-bucket
    # price move magnitude per ticker-date, recomputed here on pooled index
    # per group since diff() must stay within a ticker-date).
    cond_rows = []
    for t in TICKERS:
        sub = pooled[pooled["ticker"] == t]
        if len(sub) < 5:
            continue
        r_bvc_hidden = np.corrcoef(sub["bvc_err"], 1 - sub["gt_labelable_frac"])[0, 1] if sub["bvc_err"].std() > 0 else float("nan")
        r_lr_hidden = np.corrcoef(sub["lr_err"], 1 - sub["gt_labelable_frac"])[0, 1] if sub["lr_err"].std() > 0 else float("nan")
        cond_rows.append({"ticker": t, "n": len(sub),
                           "r_bvc_err_vs_hidden_frac": r_bvc_hidden,
                           "r_lr_err_vs_hidden_frac": r_lr_hidden})
    cond_tbl = pd.DataFrame(cond_rows)

    return per_ticker, pooled, cond_tbl, overall


def task3_noncanonical(scored_by_td, adv_divisor, window):
    counts = {}
    for (t, d), df in scored_by_td.items():
        _, n_complete = bucket_series(df, adv_divisor=adv_divisor, window=window)
        counts.setdefault(t, 0)
        counts[t] += n_complete
    return counts


def main():
    per_td, missing = load_all()
    print(f"Loaded {len(per_td)} (ticker,date) trade files; missing: {missing}")
    print()

    tbl1, by_hidden, by_spread, by_price, conc_s, conc_p = task1(per_td)
    print("=== Task 1: hidden-volume share, pooled across dates ===")
    print(tbl1.to_string(index=False))
    print(f"Rank by hidden%:        {by_hidden}")
    print(f"Rank by avg spread:     {by_spread}  (concordant: {conc_s})")
    print(f"Rank by avg price:      {by_price}  (concordant: {conc_p})")
    print("NOTE: n=5, spread/price collinear here -- directional finding, not a robust statistic.")
    print()

    tbl2, scored_by_td = task2(per_td)
    print("=== Task 2: Lee-Ready vs ground truth, 'C' trades ONLY, pooled across 7 dates ===")
    print("('E' excluded from scoring -- circular: ground truth IS which side of book 'E' hit)")
    print(tbl2.to_string(index=False))
    print()

    per_ticker3, pooled3, cond3, overall3 = task3(scored_by_td)
    print("=== Task 3 (primary): per-bucket 3-way divergence vs ground truth ===")
    print(f"Overall pooled: n_buckets={overall3['n_buckets']} "
          f"BVC MAE={overall3['bvc_mae']:.4f} LR MAE={overall3['lr_mae']:.4f}")
    print(per_ticker3.to_string(index=False))
    print("Divergence vs bucket hidden-volume fraction (correlation):")
    print(cond3.to_string(index=False))
    print()

    print("=== Task 3 (secondary, NON-CANONICAL): smaller-bucket/shorter-window VPIN counts ===")
    counts_canonical = task3_noncanonical(scored_by_td, adv_divisor=50, window=50)
    counts_small = task3_noncanonical(scored_by_td, adv_divisor=150, window=20)
    print(f"Canonical (1/50-ADV, N=50):     {counts_canonical}")
    print(f"Non-canonical (1/150-ADV, N=20): {counts_small}")

    # Persist
    tbl1.to_csv("analytics/panel_task1_hidden_volume.csv", index=False)
    tbl2.to_csv("analytics/panel_task2_lee_ready_C_only.csv", index=False)
    per_ticker3.to_csv("analytics/panel_task3_divergence.csv", index=False)
    cond3.to_csv("analytics/panel_task3_divergence_conditions.csv", index=False)
    with open("analytics/panel_task3_vpin_counts.json", "w") as f:
        json.dump({"canonical_1_50_N50": counts_canonical,
                   "noncanonical_1_150_N20": counts_small,
                   "overall_divergence": overall3}, f, indent=2, default=str)


if __name__ == "__main__":
    main()
