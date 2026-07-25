"""
ofi_panel_study.py — OFI predictive study on the clean 7-date panel.

Replaces the retired within-day 70/30 numbers (see results/OFI_STUDY.md
SUPERSEDED sections) and the void predictive numbers computed on stale data
(PROJECT_STATUS.md "Unverified/blocked" -> now retired).

Design, per instruction:
  - CROSS-REGIME split, not walk-forward: train = 4 earliest dates
    (2019-01-30, 2019-03-27, 2019-07-30, 2019-08-30), test = 3 latest
    (2019-10-30, 2019-12-30, 2020-01-30). Dates are ~2 months apart -- this is
    out-of-sample across market regimes, not a contiguous expanding window.
  - Per ticker, not pooled across tickers.
  - HAC/Newey-West standard errors on the in-sample fit (statsmodels,
    cov_type='HAC'). maxlags=5: kept consistent with this project's prior OFI
    audit convention (a handful of seconds captures short-horizon order-flow
    autocorrelation from order-splitting/iceberg replenishment; going much
    beyond that starts to eat into the effective sample size on regular-
    session data alone).
  - Contemporaneous R² per ticker on the FULL panel (all 7 dates pooled) as
    the construction check.

Reads: data/panel_{AAPL,AMZN,ETSY,NFLX,WDAY}.csv
"""

import numpy as np
import pandas as pd
import statsmodels.api as sm

TICKERS = ["AAPL", "AMZN", "ETSY", "NFLX", "WDAY"]
TRAIN_DATES = ["2019-01-30", "2019-03-27", "2019-07-30", "2019-08-30"]
TEST_DATES = ["2019-10-30", "2019-12-30", "2020-01-30"]
HAC_LAGS = 5
REGULAR_SESSION_START_SEC = 34200
REGULAR_SESSION_END_SEC = 57600
NEGLIGIBLE_R2 = 0.01   # below this, "significant" gets labeled economically negligible


def load(ticker):
    df = pd.read_csv(f"data/panel_{ticker}.csv")
    df["sec_of_day"] = df["ts"] / 1e9
    in_session = (df["sec_of_day"] >= REGULAR_SESSION_START_SEC) & \
                 (df["sec_of_day"] <= REGULAR_SESSION_END_SEC)
    valid_quote = (df["best_bid"] > 0) & (df["best_ask"] > 0) & \
                  (df["best_ask"] > df["best_bid"]) & \
                  (df["quoted_spread"] <= 0.10 * df["mid_price"])
    df = df[in_session & valid_quote].copy()
    df = df.sort_values(["date", "ts"]).reset_index(drop=True)
    # Returns computed WITHIN each date only -- never bridge a session boundary.
    df["ret"] = df.groupby("date")["mid_price"].pct_change()
    df["fwd_ret_1s"] = df.groupby("date")["mid_price"].pct_change().shift(-1)
    # shift(-1) can leak the next date's first row into the previous date's
    # last row; null out any fwd_ret whose row is the last of its date.
    last_of_date = df.groupby("date").cumcount(ascending=False) == 0
    df.loc[last_of_date, "fwd_ret_1s"] = np.nan
    return df


def contemporaneous_r2(df):
    d = df.dropna(subset=["ofi", "ret"])
    if len(d) < 30:
        return None, len(d)
    X = sm.add_constant(d["ofi"].values)
    model = sm.OLS(d["ret"].values, X).fit()
    return model.rsquared, len(d)


def fit_hac(train_df):
    d = train_df.dropna(subset=["ofi", "fwd_ret_1s"])
    X = sm.add_constant(d["ofi"].values)
    y = d["fwd_ret_1s"].values
    model = sm.OLS(y, X).fit(cov_type="HAC", cov_kwds={"maxlags": HAC_LAGS})
    return model, len(d)


def oos_r2(model, test_df):
    d = test_df.dropna(subset=["ofi", "fwd_ret_1s"])
    if len(d) < 5:
        return None, len(d)
    X = sm.add_constant(d["ofi"].values, has_constant="add")
    y = d["fwd_ret_1s"].values
    yhat = model.predict(X)
    ss_res = np.sum((y - yhat) ** 2)
    ss_tot = np.sum((y - y.mean()) ** 2)
    if ss_tot == 0:
        return None, len(d)
    return 1 - ss_res / ss_tot, len(d)


def main():
    print("=== Contemporaneous OFI construction check, full 7-date panel ===")
    contemp_rows = []
    panels = {}
    for t in TICKERS:
        df = load(t)
        panels[t] = df
        r2, n = contemporaneous_r2(df)
        contemp_rows.append({"ticker": t, "contemporaneous_r2": r2, "n": n})
    contemp_tbl = pd.DataFrame(contemp_rows)
    print(contemp_tbl.to_string(index=False))
    print()

    print("=== Predictive OFI -> fwd_ret_1s, CROSS-REGIME split ===")
    print(f"Train dates: {TRAIN_DATES}")
    print(f"Test dates:  {TEST_DATES}")
    print(f"HAC lags: {HAC_LAGS}\n")

    summary_rows = []
    per_date_rows = []
    for t in TICKERS:
        df = panels[t]
        train = df[df["date"].isin(TRAIN_DATES)]
        test = df[df["date"].isin(TEST_DATES)]

        model, n_train = fit_hac(train)
        beta = model.params[1]
        se = model.bse[1]
        pval = model.pvalues[1]
        r2_in = model.rsquared

        r2_out_pooled, n_test_pooled = oos_r2(model, test)

        for d in TEST_DATES:
            sub = test[test["date"] == d]
            r2_d, n_d = oos_r2(model, sub)
            per_date_rows.append({"ticker": t, "test_date": d, "r2_oos": r2_d, "n": n_d})

        significant = pval < 0.05
        negligible = (r2_out_pooled is not None) and (abs(r2_out_pooled) < NEGLIGIBLE_R2)
        summary_rows.append({
            "ticker": t, "n_train": n_train, "beta": beta, "hac_se": se, "p_value": pval,
            "r2_in_sample": r2_in, "n_test_pooled": n_test_pooled,
            "r2_oos_pooled": r2_out_pooled,
            "significant_p<0.05": significant,
            "economically_negligible": negligible if r2_out_pooled is not None else None,
        })

    summary_tbl = pd.DataFrame(summary_rows)
    per_date_tbl = pd.DataFrame(per_date_rows)
    print(summary_tbl.to_string(index=False))
    print()
    print("=== Per-test-date OOS R² (regime dependence, not averaged away) ===")
    print(per_date_tbl.pivot(index="ticker", columns="test_date", values="r2_oos").to_string())

    contemp_tbl.to_csv("results/panel_contemporaneous_r2.csv", index=False)
    summary_tbl.to_csv("results/panel_predictive_summary.csv", index=False)
    per_date_tbl.to_csv("results/panel_predictive_per_date.csv", index=False)


if __name__ == "__main__":
    main()
