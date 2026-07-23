"""
build_panel.py — aggregate per-day feature CSVs into panel datasets.

Reads:  data/features_AAPL_MMDDYYYY.csv  (any number of dates)
        data/features_AMZN_MMDDYYYY.csv

Writes: data/panel_AAPL.csv
        data/panel_AMZN.csv

Each panel CSV has all the original feature columns plus:
  date             — YYYY-MM-DD string
  market_condition — normal | high_vol | low_vol | earnings | unknown

Usage:
    python scripts/build_panel.py [--data-dir data/]
"""

import sys
import re
from pathlib import Path
from datetime import datetime

import pandas as pd

MARKET_CONDITIONS: dict[str, str] = {
    "01302019": "normal",
    "03272019": "normal",
    "07302019": "normal",
    "08302019": "normal",
    "10302019": "normal",
    "12302019": "normal",
    "01302020": "normal",
}

DATE_RE = re.compile(r"features_[A-Z]+_(\d{8})\.csv$")


def parse_date(itch_date: str) -> str:
    """Convert MMDDYYYY → YYYY-MM-DD."""
    dt = datetime.strptime(itch_date, "%m%d%Y")
    return dt.strftime("%Y-%m-%d")


def load_day(path: Path) -> pd.DataFrame | None:
    """Load one per-day feature CSV, inject date/market_condition columns."""
    m = DATE_RE.search(path.name)
    if m is None:
        print(f"  WARN: cannot parse date from {path.name} — skipping", file=sys.stderr)
        return None

    itch_date = m.group(1)
    date_str = parse_date(itch_date)
    condition = MARKET_CONDITIONS.get(itch_date, "unknown")

    df = pd.read_csv(path)
    df.insert(0, "date", date_str)
    df.insert(1, "market_condition", condition)
    return df


def build_panel(data_dir: Path, ticker: str) -> pd.DataFrame | None:
    pattern = f"features_{ticker}_*.csv"
    files = sorted(data_dir.glob(pattern))

    if not files:
        print(f"No files matching {pattern} in {data_dir}", file=sys.stderr)
        return None

    print(f"\n{ticker}: found {len(files)} day(s)")
    frames = []
    for f in files:
        df = load_day(f)
        if df is not None:
            frames.append(df)
            print(f"  {f.name}  rows={len(df)}  cond={df['market_condition'].iloc[0]}")

    if not frames:
        return None

    panel = pd.concat(frames, ignore_index=True)
    panel.sort_values(["date", "ts"], inplace=True)
    panel.reset_index(drop=True, inplace=True)
    return panel


def main() -> None:
    data_dir = Path("data")
    for i, arg in enumerate(sys.argv[1:]):
        if arg == "--data-dir" and i + 2 <= len(sys.argv) - 1:
            data_dir = Path(sys.argv[i + 2])

    if not data_dir.exists():
        print(f"ERROR: data dir not found: {data_dir}", file=sys.stderr)
        sys.exit(1)

    for ticker in ("AAPL", "AMZN", "ETSY", "NFLX", "WDAY"):
        panel = build_panel(data_dir, ticker)
        if panel is None:
            continue

        out_path = data_dir / f"panel_{ticker}.csv"
        panel.to_csv(out_path, index=False)
        print(f"  → {out_path}  total rows={len(panel)}")

    print("\nDone. Next step: python scripts/panel_analysis.py")


if __name__ == "__main__":
    main()
