"""
validate_book.py — automatic reconstructed-book sanity check.

Prints reconstructed best bid/ask/spread for a sample of rows per CSV and
hard-fails on the two things that are never real: a negative (crossed) spread,
and a spread matching the exact numeric signature of the 2026-07-24
uint32_t-underflow bug (see results/OFI_STUDY.md, "Blocker 1") — every CSV in
data/ showed a ~$429,492 "spread" for months because nothing checked. This
script is invoked automatically by regenerate_data.sh; it is not meant to be
something a human has to remember to run by hand.

2026-07-24, revised after running against real BX venue data: a naive
"spread must be sub-dollar" hard-fail is WRONG for real, legitimately thin
venues. AAPL/MSFT reconstructed from a real BX sample file showed 309 and 121
rows (of ~14,500) with spread > $1, up to $27.85 on a ~$208 stock, clustered
at session open/close where a small-venue book can be genuinely one-sided or
near-empty. That is real, correctly-reconstructed thin-book behavior, not
corruption — and is expected per the venue's own low volume, not a defect.
The check below distinguishes the two instead of conflating them:
  - HARD FAIL: negative spread (a real crossed book is never valid).
  - HARD FAIL: any spread within UNDERFLOW_SIGNATURE_BAND of the exact
    uint32_t-wraparound constant (2^32/10000 = $429,496.7296) — this is the
    known, specific numeric fingerprint of the original bug, not a generic
    "big number" heuristic.
  - HARD FAIL: any spread > ABSURD_SPREAD_DOLLARS ($1,000) — no real
    single-name equity spread is ever this wide, regardless of venue or
    liquidity; catches a *different* magnitude bug of the same class without
    being overfit to the exact old constant.
  - INFORMATIONAL, not fatal: spread > MAX_SANE_SPREAD_DOLLARS ($1) but below
    the absurd ceiling. Printed with count/percentage so a human can eyeball
    it (this is what a thin/sparse-venue book near session boundaries looks
    like), but does not block promotion by itself.

bid/ask are reconstructed exactly (not approximated) from the CSV's own
mid_price and quoted_spread columns: since mid_price = (bid+ask)/2 and
quoted_spread = ask-bid by construction (feature_engine.cpp), bid and ask
solve uniquely as mid -+ spread/2.

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
SAMPLE_SIZE = 10


def validate(csv_path: str) -> bool:
    df = pd.read_csv(csv_path)
    print(f"\n== {csv_path} ({len(df)} rows) ==")
    if df.empty:
        print("  EMPTY — nothing to validate")
        return True

    spread = df['quoted_spread']
    mid = df['mid_price']
    bid = mid - spread / 2.0
    ask = mid + spread / 2.0

    sample_n = min(SAMPLE_SIZE, len(df))
    sample = df.sample(sample_n, random_state=0).sort_index()
    print(f"  sample of {sample_n} rows (bid/ask reconstructed from mid_price +- spread/2):")
    for i in sample.index:
        print(f"    ts={df.loc[i, 'ts']:>16}  bid={bid[i]:>10.4f}  "
              f"ask={ask[i]:>10.4f}  spread={spread[i]:>10.4f}")

    n_total = len(df)
    n_negative = int((spread < 0).sum())
    n_underflow_signature = int(
        ((spread - UNDERFLOW_SIGNATURE_CENTER).abs() <= UNDERFLOW_SIGNATURE_BAND).sum()
    )
    n_absurd = int((spread > ABSURD_SPREAD_DOLLARS).sum())
    n_wide = int(((spread > MAX_SANE_SPREAD_DOLLARS) & (spread <= ABSURD_SPREAD_DOLLARS)).sum())

    ok = True
    if n_negative > 0:
        print(f"  FAIL: {n_negative}/{n_total} rows have NEGATIVE spread (crossed book: ask < bid)")
        ok = False
    if n_underflow_signature > 0:
        print(f"  FAIL: {n_underflow_signature}/{n_total} rows have spread within "
              f"${UNDERFLOW_SIGNATURE_BAND:.0f} of ${UNDERFLOW_SIGNATURE_CENTER:,.2f} — the exact "
              f"numeric signature of the 2026-07-24 uint32_t-underflow bug (a crossed book "
              f"wraps around to this specific huge POSITIVE value). Do not treat this as benign.")
        ok = False
    if n_absurd > 0:
        print(f"  FAIL: {n_absurd}/{n_total} rows have spread > ${ABSURD_SPREAD_DOLLARS:,.2f} "
              f"(max={spread.max():.2f}) — no real single-name equity spread is ever this wide.")
        ok = False
    if n_wide > 0:
        print(f"  INFO: {n_wide}/{n_total} rows ({100*n_wide/n_total:.1f}%) have spread > "
              f"${MAX_SANE_SPREAD_DOLLARS:.2f} but below the absurd ceiling (median of these: "
              f"${spread[(spread > MAX_SANE_SPREAD_DOLLARS) & (spread <= ABSURD_SPREAD_DOLLARS)].median():.2f}, "
              f"max ${spread[(spread > MAX_SANE_SPREAD_DOLLARS) & (spread <= ABSURD_SPREAD_DOLLARS)].max():.2f}). "
              f"NOT treated as a failure — this is expected on thin/low-volume venues, "
              f"especially near session open/close. Eyeball the sample above if unsure.")
    if ok and n_wide == 0:
        print(f"  PASS: all {n_total} rows have 0 <= spread <= ${MAX_SANE_SPREAD_DOLLARS:.2f}")
    elif ok:
        print(f"  PASS (with informational wide-spread rows noted above): "
              f"no negative, no underflow-signature, no absurd (>${ABSURD_SPREAD_DOLLARS:,.0f}) spreads")
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
