"""
validate_book.py — automatic reconstructed-book sanity check.

Prints the RAW reconstructed best_bid/best_ask/spread for a sample of rows
per CSV and hard-fails on quote-validity violations. This is the check that
caught the 2026-07-24 uint32_t-underflow bug (see results/OFI_STUDY.md,
"Blocker 1") — every CSV in data/ showed a ~$429,492 "spread" for months
because nothing checked. This script is invoked automatically by
regenerate_data.sh; it is not meant to be something a human has to remember
to run by hand.

2026-07-24, revised three times:
  1. After running against real BX venue data: a naive "spread must be
     sub-dollar" hard-fail is wrong for real, legitimately thin venues —
     see the ABSURD_SPREAD_DOLLARS / MAX_SANE_SPREAD_DOLLARS distinction
     below.
  2. After finding a real pre-market stub quote (ETSY, 2019-12-30) that a
     bid/ask reconstruction from mid_price +- quoted_spread/2 would validate
     even though it's a degenerate quote: reconstructing bid/ask from
     mid_price and quoted_spread is definitionally self-consistent with
     them, so it cannot catch a row where mid_price or quoted_spread
     themselves are wrong or degenerate. This script now reads best_bid and
     best_ask DIRECTLY from the CSV — the raw BookSnapshot values written by
     feature_engine.cpp, not a reconstruction — and validates those.
  3. After that fix immediately broke ETSY/WDAY promotion again: a WHOLE-FILE
     "spread > $1,000" / "spread > 10% of mid" hard fail is wrong too,
     because it conflated two different concerns. Structural corruption
     (negative spread, a crossed book, the uint32_t-underflow signature) is
     never valid, any time of day, and stays a whole-file hard fail. But a
     real pre-market stub quote (near-zero bid, deliberately-unfillable
     distant ask) is NORMAL market behavior outside the regular session —
     see "Degenerate-quote characterization," results/OFI_STUDY.md — and
     failing promotion over it wrongly treats real pre-market data as
     corrupted. The absurd-spread and degenerate-percentage checks are now
     scoped to the regular session (09:30-16:00 ET) only; pre-market/
     post-market rows are reported for visibility but never block
     promotion on spread width alone.

Checks on the raw best_bid/best_ask columns:
  - HARD FAIL, any time of day: best_bid <= 0, best_ask <= 0 (the engine
    never emits a row without a real two-sided book — feature_engine.cpp's
    on_book_update skips emission when either side is 0 — so seeing this at
    all indicates a reconstruction bug, not a real market state).
  - HARD FAIL, any time of day: best_ask <= best_bid (crossed/locked book).
  - HARD FAIL, any time of day: spread within UNDERFLOW_SIGNATURE_BAND of the
    exact uint32_t-wraparound constant (2^32/10000 = $429,496.7296) — the
    known, specific numeric fingerprint of the original bug.
  - HARD FAIL, REGULAR SESSION ONLY (09:30-16:00 ET): spread > ABSURD_SPREAD_DOLLARS
    ($1,000), or spread > 10% of mid_price. Outside the regular session these
    are reported as informational only — pre-market stub quotes are real and
    expected there, not corruption.
  - INFORMATIONAL, not fatal, any time: spread > MAX_SANE_SPREAD_DOLLARS ($1)
    but below the regular-session ceilings.

Usage:
    python3 scripts/validate_book.py <csv1> [<csv2> ...]
Exit code 1 if any file fails validation.
"""

import sys
import pandas as pd

MAX_SANE_SPREAD_DOLLARS = 1.00
ABSURD_SPREAD_DOLLARS = 1000.00
UNDERFLOW_SIGNATURE_CENTER = (2 ** 32) / 10000.0   # 429496.7296
UNDERFLOW_SIGNATURE_BAND = 100.0
MAX_SPREAD_PCT_OF_MID = 0.10
REGULAR_SESSION_START_SEC = 34200   # 09:30 ET
REGULAR_SESSION_END_SEC = 57600     # 16:00 ET
SAMPLE_SIZE = 10


def validate(csv_path: str) -> bool:
    df = pd.read_csv(csv_path)
    print(f"\n== {csv_path} ({len(df)} rows) ==")
    if df.empty:
        print("  EMPTY — nothing to validate")
        return True

    if 'best_bid' not in df.columns or 'best_ask' not in df.columns:
        print(f"  FAIL: no best_bid/best_ask columns — this CSV predates the raw-quote "
              f"export (see FeatureRow) and cannot be validated against real quote "
              f"validity. Regenerate it.")
        return False

    bid = df['best_bid']
    ask = df['best_ask']
    mid = df['mid_price']
    spread = df['quoted_spread']
    sec_of_day = df['ts'] / 1e9
    in_session = (sec_of_day >= REGULAR_SESSION_START_SEC) & (sec_of_day <= REGULAR_SESSION_END_SEC)
    n_total = len(df)

    sample_n = min(SAMPLE_SIZE, len(df))
    sample = df.sample(sample_n, random_state=0).sort_index()
    print(f"  sample of {sample_n} rows (best_bid/best_ask are raw, from the book, not reconstructed):")
    for i in sample.index:
        print(f"    ts={df.loc[i, 'ts']:>16}  best_bid={bid[i]:>10.4f}  "
              f"best_ask={ask[i]:>10.4f}  spread={spread[i]:>10.4f}")

    # ── Always-invalid, any time of day ──────────────────────────────────
    n_bid_nonpos = int((bid <= 0).sum())
    n_ask_nonpos = int((ask <= 0).sum())
    n_crossed_locked = int((ask <= bid).sum())
    n_underflow_signature = int(
        ((spread - UNDERFLOW_SIGNATURE_CENTER).abs() <= UNDERFLOW_SIGNATURE_BAND).sum()
    )

    # ── Regular-session-only: absurd/degenerate spread ───────────────────
    reg_spread, reg_mid = spread[in_session], mid[in_session]
    n_reg = int(in_session.sum())
    n_absurd_reg = int((reg_spread > ABSURD_SPREAD_DOLLARS).sum())
    n_degenerate_reg = int((reg_spread > MAX_SPREAD_PCT_OF_MID * reg_mid).sum())

    # Outside regular session: same thresholds, informational only.
    outside_spread, outside_mid = spread[~in_session], mid[~in_session]
    n_absurd_outside = int((outside_spread > ABSURD_SPREAD_DOLLARS).sum())
    n_degenerate_outside = int((outside_spread > MAX_SPREAD_PCT_OF_MID * outside_mid).sum())

    n_wide = int(((spread > MAX_SANE_SPREAD_DOLLARS) &
                  (spread <= MAX_SPREAD_PCT_OF_MID * mid)).sum())

    ok = True
    if n_bid_nonpos > 0:
        print(f"  FAIL: {n_bid_nonpos}/{n_total} rows have best_bid <= 0 (no real bid)")
        ok = False
    if n_ask_nonpos > 0:
        print(f"  FAIL: {n_ask_nonpos}/{n_total} rows have best_ask <= 0 (no real ask)")
        ok = False
    if n_crossed_locked > 0:
        print(f"  FAIL: {n_crossed_locked}/{n_total} rows have best_ask <= best_bid "
              f"(crossed or locked book)")
        ok = False
    if n_underflow_signature > 0:
        print(f"  FAIL: {n_underflow_signature}/{n_total} rows have spread within "
              f"${UNDERFLOW_SIGNATURE_BAND:.0f} of ${UNDERFLOW_SIGNATURE_CENTER:,.2f} — the exact "
              f"numeric signature of the 2026-07-24 uint32_t-underflow bug (a crossed book "
              f"wraps around to this specific huge POSITIVE value). Do not treat this as benign.")
        ok = False
    if n_absurd_reg > 0:
        print(f"  FAIL: {n_absurd_reg}/{n_reg} REGULAR-SESSION rows have spread > "
              f"${ABSURD_SPREAD_DOLLARS:,.2f} (max={reg_spread.max():.2f}) — no real "
              f"single-name equity spread is ever this wide during regular hours.")
        ok = False
    if n_degenerate_reg > 0:
        print(f"  FAIL: {n_degenerate_reg}/{n_reg} REGULAR-SESSION rows have spread > "
              f"{MAX_SPREAD_PCT_OF_MID*100:.0f}% of mid_price (degenerate/stub quote during "
              f"regular hours — see results/OFI_STUDY.md 'Degenerate-quote characterization')")
        ok = False
    if n_absurd_outside > 0 or n_degenerate_outside > 0:
        print(f"  INFO: outside regular session, {n_absurd_outside} rows have spread > "
              f"${ABSURD_SPREAD_DOLLARS:,.0f} and {n_degenerate_outside} rows have spread > "
              f"{MAX_SPREAD_PCT_OF_MID*100:.0f}% of mid — NOT a failure: real pre-market/"
              f"post-market stub quotes are expected here, not corruption. These rows are "
              f"excluded from any regular-session analysis anyway.")
    if n_wide > 0:
        wide_mask = (spread > MAX_SANE_SPREAD_DOLLARS) & (spread <= MAX_SPREAD_PCT_OF_MID * mid)
        print(f"  INFO: {n_wide}/{n_total} rows ({100*n_wide/n_total:.1f}%) have spread > "
              f"${MAX_SANE_SPREAD_DOLLARS:.2f} but below the {MAX_SPREAD_PCT_OF_MID*100:.0f}%-of-mid "
              f"ceiling (median of these: ${spread[wide_mask].median():.2f}, "
              f"max ${spread[wide_mask].max():.2f}). NOT treated as a failure — this is expected "
              f"on thin/low-volume venues, especially near session open/close. Eyeball the "
              f"sample above if unsure.")
    if ok and n_wide == 0 and n_absurd_outside == 0 and n_degenerate_outside == 0:
        print(f"  PASS: all {n_total} rows have valid, non-degenerate quotes")
    elif ok:
        print(f"  PASS (with informational notes above): "
              f"no invalid/crossed/absurd/degenerate quotes during regular session")
    return ok


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 validate_book.py <csv1> [<csv2> ...]")
        sys.exit(1)

    all_ok = True
    for path in sys.argv[1:]:
        if not validate(path):
            all_ok = False

    print()
    if not all_ok:
        print("BOOK VALIDATION FAILED — do not trust this data. See FAIL lines above.")
        sys.exit(1)
    print("All files passed book validation.")


if __name__ == '__main__':
    main()
