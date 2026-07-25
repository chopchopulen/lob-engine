"""
vpin_pipeline.py — Task 1b (coverage characterization), Task 2 (Lee-Ready
scoring against ground truth), Task 3 (VPIN, 3 variants) for the VPIN/Lee-Ready
analytics layer. Offline analytics only — reads analytics/trades/trades_*.csv
(produced by the vpin_extract binary), writes scored/bucketed CSVs plus the
numbers that feed analytics/VPIN_RESULTS.md. Does not touch the hot path,
matching engine, or bench/BASELINE.md.

Single date (2026-07-25 run: 12/30/2019) across all 5 panel tickers.
"""

import sys
import math
import pandas as pd
import numpy as np

TICKERS = ["AAPL", "AMZN", "ETSY", "NFLX", "WDAY"]
DATE = "12302019"
TRADES_DIR = "analytics/trades"
REGULAR_SESSION_START_SEC = 34200   # 09:30 ET
REGULAR_SESSION_END_SEC = 57600     # 16:00 ET
N_BUCKETS_ADV = 50   # bucket size = regular-session executed volume / 50
VPIN_WINDOW = 50      # rolling window, in buckets


def load(ticker):
    df = pd.read_csv(f"{TRADES_DIR}/trades_{ticker}_{DATE}.csv")
    df["sec_of_day"] = df["ts"] / 1e9
    df["in_session"] = (df["sec_of_day"] >= REGULAR_SESSION_START_SEC) & \
                        (df["sec_of_day"] <= REGULAR_SESSION_END_SEC)
    # Non-printable 'C' rows: real book mutation already happened upstream in
    # vpin_extract, but per ITCH 5.0 spec 1.4.2 these shares reappear in a
    # later bulk print — exclude from volume/labeling here to avoid double count.
    df["countable"] = df["printable"] == "Y"
    df["price_d"] = df["price"] / 10000.0
    df["bid_d"] = df["prevailing_bid"] / 10000.0
    df["ask_d"] = df["prevailing_ask"] / 10000.0
    df["mid_d"] = (df["bid_d"] + df["ask_d"]) / 2.0
    df["spread_d"] = df["ask_d"] - df["bid_d"]
    return df


# ── Task 1b: coverage / hidden-volume characterization ───────────────────────

def task1b(dfs):
    rows = []
    for t, df in dfs.items():
        reg = df[df["in_session"] & df["countable"]]
        total_vol = reg["shares"].sum()
        gt_vol = reg.loc[reg["gt_side"] != "U", "shares"].sum()
        hidden_vol = total_vol - gt_vol
        gt_gap_pct = 100.0 * hidden_vol / total_vol if total_vol else float("nan")
        med_spread = reg.loc[(reg["bid_d"] > 0) & (reg["ask_d"] > 0), "spread_d"].median()
        med_price = reg["price_d"].median()
        rows.append({
            "ticker": t, "total_vol": int(total_vol), "hidden_vol": int(hidden_vol),
            "gt_gap_pct": gt_gap_pct, "median_spread": med_spread, "median_price": med_price,
        })
    tbl = pd.DataFrame(rows)

    def pearson(a, b):
        a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
        if len(a) < 3:
            return float("nan")
        return float(np.corrcoef(a, b)[0, 1])

    r_spread = pearson(tbl["gt_gap_pct"], tbl["median_spread"])
    r_price = pearson(tbl["gt_gap_pct"], tbl["median_price"])
    return tbl, r_spread, r_price


# ── Task 2: Lee-Ready classification + scoring ────────────────────────────────

def lee_ready(df):
    """Contemporaneous quote rule + tick-rule fallback, zero-tick inherits.
    No 5-second lag: this is a single sequenced feed, not a consolidated tape
    (the original 1991 lag compensates for asynchronous trade/quote reporting
    that doesn't apply here)."""
    lr_side = []
    method = []   # 'quote', 'tick', 'inherit'
    prev_price = None
    prev_label = None
    for _, row in df.iterrows():
        price, mid = row["price_d"], row["mid_d"]
        if row["bid_d"] <= 0 or row["ask_d"] <= 0:
            # no valid quote (e.g. pre-market stub) — fall back to tick rule directly
            if prev_price is None or price == prev_price:
                lbl = prev_label if prev_label else "U"
                m = "inherit"
            else:
                lbl = "B" if price > prev_price else "S"
                m = "tick"
        elif price > mid:
            lbl, m = "B", "quote"
        elif price < mid:
            lbl, m = "S", "quote"
        else:
            # exactly at midpoint -> tick rule fallback
            if prev_price is None or price == prev_price:
                lbl = prev_label if prev_label else "U"
                m = "inherit"
            else:
                lbl = "B" if price > prev_price else "S"
                m = "tick"
        lr_side.append(lbl)
        method.append(m)
        prev_price = price
        prev_label = lbl
    df = df.copy()
    df["lr_side"] = lr_side
    df["lr_method"] = method
    return df


def task2(dfs):
    scored = {}
    summary_rows = []
    for t, df in dfs.items():
        d = df[df["in_session"] & df["countable"]].sort_values("ts").reset_index(drop=True)
        d = lee_ready(d)
        scored[t] = d
        lit = d[d["gt_side"] != "U"]
        n = len(lit)
        agree = (lit["lr_side"] == lit["gt_side"]).sum()
        overall = 100.0 * agree / n if n else float("nan")

        # STRUCTURAL CAVEAT, not a scoring bug: 'E' (Order Executed) fills at
        # the resting order's own displayed price, which is exactly the
        # prevailing bid or ask this reconstruction just read off the same
        # book state -- so quote-rule vs ground-truth is near-tautological
        # for 'E' (no lag, no price improvement possible in this feed).
        # 'C' (Order Executed With Price) genuinely diverges from the resting
        # display price -- that subset is the real test of Lee-Ready here.
        e_lit = lit[lit["event_type"] == "E"]
        c_lit = lit[lit["event_type"] == "C"]
        e_agree_pct = 100.0 * (e_lit["lr_side"] == e_lit["gt_side"]).sum() / len(e_lit) if len(e_lit) else float("nan")
        c_agree_pct = 100.0 * (c_lit["lr_side"] == c_lit["gt_side"]).sum() / len(c_lit) if len(c_lit) else float("nan")

        by_method = {}
        for m in ["quote", "tick", "inherit"]:
            sub = lit[lit["lr_method"] == m]
            by_method[m] = {
                "n": len(sub),
                "agree_pct": 100.0 * (sub["lr_side"] == sub["gt_side"]).sum() / len(sub) if len(sub) else float("nan"),
            }

        at_mid = lit[lit["price_d"] == lit["mid_d"]]
        at_mid_pct = 100.0 * len(at_mid) / n if n else float("nan")

        # spread-width quartiles (on lit trades with valid quotes)
        lit_q = lit[(lit["bid_d"] > 0) & (lit["ask_d"] > 0)].copy()
        quart_acc = []
        if len(lit_q) >= 4:
            lit_q["spread_q"] = pd.qcut(lit_q["spread_d"], 4, labels=False, duplicates="drop")
            for q in sorted(lit_q["spread_q"].dropna().unique()):
                sub = lit_q[lit_q["spread_q"] == q]
                quart_acc.append({
                    "quartile": int(q) + 1,
                    "n": len(sub),
                    "agree_pct": 100.0 * (sub["lr_side"] == sub["gt_side"]).sum() / len(sub),
                })

        summary_rows.append({
            "ticker": t, "n_lit": n, "overall_agree_pct": overall,
            "e_n": len(e_lit), "e_agree_pct": e_agree_pct,
            "c_n": len(c_lit), "c_agree_pct": c_agree_pct,
            "quote_n": by_method["quote"]["n"], "quote_agree_pct": by_method["quote"]["agree_pct"],
            "tick_n": by_method["tick"]["n"], "tick_agree_pct": by_method["tick"]["agree_pct"],
            "inherit_n": by_method["inherit"]["n"], "inherit_agree_pct": by_method["inherit"]["agree_pct"],
            "at_midpoint_pct": at_mid_pct,
            "spread_quartile_acc": quart_acc,
        })
    return scored, pd.DataFrame(summary_rows)


# ── Task 3: VPIN, 3 variants ─────────────────────────────────────────────────

def bucket_and_vpin(df):
    """df: in-session, countable, LR-scored trades for one ticker, sorted by ts.
    Buckets close at cumulative volume >= bucket_size; last partial bucket
    discarded, never carried across the session boundary (there is only one
    session per file here, so this is the whole-file tail)."""
    total_vol = df["shares"].sum()
    if total_vol == 0:
        return None
    bucket_size = total_vol / N_BUCKETS_ADV

    buckets = []
    cum = 0
    b_gt_buy = b_gt_sell = b_gt_unlabeled = 0
    b_lr_buy = b_lr_sell = 0
    b_vol = 0
    b_last_price = None
    b_first_price = None

    for _, row in df.iterrows():
        shares, price = row["shares"], row["price_d"]
        if b_first_price is None:
            b_first_price = price
        b_last_price = price
        if row["gt_side"] == "B":
            b_gt_buy += shares
        elif row["gt_side"] == "S":
            b_gt_sell += shares
        else:
            b_gt_unlabeled += shares
        if row["lr_side"] == "B":
            b_lr_buy += shares
        elif row["lr_side"] == "S":
            b_lr_sell += shares
        b_vol += shares
        cum += shares
        if cum >= bucket_size:
            buckets.append({
                "vol": b_vol, "close_price": b_last_price,
                "gt_buy": b_gt_buy, "gt_sell": b_gt_sell, "gt_unlabeled": b_gt_unlabeled,
                "lr_buy": b_lr_buy, "lr_sell": b_lr_sell,
            })
            cum = 0
            b_gt_buy = b_gt_sell = b_gt_unlabeled = 0
            b_lr_buy = b_lr_sell = 0
            b_vol = 0
            b_first_price = None
    # trailing partial bucket (cum < bucket_size) is discarded here by falling
    # off the loop without being appended.

    if not buckets:
        return {"n_buckets": 0, "n_vpin_windows": 0, "bucket_size": bucket_size}

    bt = pd.DataFrame(buckets)
    # BVC: standardize bucket-over-bucket price change through normal CDF
    price_changes = bt["close_price"].diff()
    sigma = price_changes.std(ddof=0)
    if sigma == 0 or math.isnan(sigma):
        z = pd.Series([0.5] * len(bt))
    else:
        z = price_changes.apply(lambda dp: 0.5 if pd.isna(dp) else
                                 0.5 * (1 + math.erf((dp / sigma) / math.sqrt(2))))
    bt["bvc_buy"] = bt["vol"] * z
    bt["bvc_sell"] = bt["vol"] * (1 - z)

    def vpin_series(buy, sell, vol):
        imb = (buy - sell).abs()
        roll_imb = imb.rolling(VPIN_WINDOW).sum()
        roll_vol = vol.rolling(VPIN_WINDOW).sum()
        return roll_imb / roll_vol

    vpin_gt = vpin_series(bt["gt_buy"], bt["gt_sell"], bt["vol"])
    vpin_lr = vpin_series(bt["lr_buy"], bt["lr_sell"], bt["vol"])
    vpin_bvc = vpin_series(bt["bvc_buy"], bt["bvc_sell"], bt["vol"])

    n_complete = int(vpin_gt.notna().sum())  # same for all 3 (same window/vol)

    return {
        "n_buckets": len(bt),
        "bucket_size": bucket_size,
        "n_vpin_windows": n_complete,
        "vpin_gt": [v for v in vpin_gt.dropna().tolist()],
        "vpin_lr": [v for v in vpin_lr.dropna().tolist()],
        "vpin_bvc": [v for v in vpin_bvc.dropna().tolist()],
    }


def main():
    dfs = {t: load(t) for t in TICKERS}

    tbl1b, r_spread, r_price = task1b(dfs)
    print("=== Task 1b: coverage / hidden-volume characterization ===")
    print(tbl1b.to_string(index=False))
    print(f"Pearson r(gt_gap_pct, median_spread) = {r_spread:.3f}")
    print(f"Pearson r(gt_gap_pct, median_price)  = {r_price:.3f}")
    print()

    scored, tbl2 = task2(dfs)
    print("=== Task 2: Lee-Ready vs ground truth ===")
    print("NOTE: overall_agree_pct is near-tautological -- see e_agree_pct vs c_agree_pct.")
    print("'E' fills at the resting order's own displayed price = the exact prevailing")
    print("quote just read from the same book state (no lag, no price improvement in")
    print("this feed), so quote-rule mechanically recovers ground truth for 'E'. 'C'")
    print("(price diverges from resting price) is the real test of Lee-Ready here.")
    print(tbl2.drop(columns=["spread_quartile_acc"]).to_string(index=False))
    for _, r in tbl2.iterrows():
        print(f"  {r['ticker']} spread-quartile accuracy: {r['spread_quartile_acc']}")
    print()

    print("=== Task 3: VPIN, 3 variants (plumbing check) ===")
    vpin_results = {}
    for t in TICKERS:
        res = bucket_and_vpin(scored[t])
        vpin_results[t] = res
        print(f"  {t}: buckets={res['n_buckets']} bucket_size≈{res.get('bucket_size', 0):.0f} shares "
              f"complete_vpin_windows={res['n_vpin_windows']}")
        if res['n_vpin_windows'] > 0:
            print(f"    GT VPIN:  {[round(v,4) for v in res['vpin_gt']]}")
            print(f"    LR VPIN:  {[round(v,4) for v in res['vpin_lr']]}")
            print(f"    BVC VPIN: {[round(v,4) for v in res['vpin_bvc']]}")

    # Persist for the report writer / reproducibility
    tbl1b.to_csv("analytics/task1b_coverage.csv", index=False)
    tbl2.drop(columns=["spread_quartile_acc"]).to_csv("analytics/task2_lee_ready_summary.csv", index=False)

    import json
    with open("analytics/task3_vpin_results.json", "w") as f:
        json.dump(vpin_results, f, indent=2, default=str)


if __name__ == "__main__":
    main()
