"""
validate_book.py — automatic reconstructed-book sanity check.

Prints reconstructed best bid/ask/spread for a sample of rows per CSV and
asserts spreads are positive, non-crossed, and sub-dollar. This is the exact
check that caught the 2026-07-24 stale-data bug (see results/OFI_STUDY.md,
"Blocker 1") — every CSV in data/ showed a ~$429,492 "spread" via a
uint32_t underflow, for months, because nothing checked. This script is
invoked automatically by regenerate_data.sh after every regeneration; it is
not meant to be something a human has to remember to run by hand.

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
    n_wide = int((spread > MAX_SANE_SPREAD_DOLLARS).sum())

    ok = True
    if n_negative > 0:
        print(f"  FAIL: {n_negative}/{n_total} rows have NEGATIVE spread (crossed book: ask < bid)")
        ok = False
    if n_wide > 0:
        print(f"  FAIL: {n_wide}/{n_total} rows have spread > ${MAX_SANE_SPREAD_DOLLARS:.2f} "
              f"(median={spread.median():.2f}, max={spread.max():.2f}). This is the exact "
              f"signature of the 2026-07-24 uint32_t-underflow bug (a crossed book wraps "
              f"around to a huge POSITIVE value, not a visible negative one) — do not treat "
              f"a large positive spread as benign.")
        ok = False
    if ok:
        print(f"  PASS: all {n_total} rows have 0 <= spread <= ${MAX_SANE_SPREAD_DOLLARS:.2f}")
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
