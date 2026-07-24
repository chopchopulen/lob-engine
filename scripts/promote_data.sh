#!/usr/bin/env bash
set -euo pipefail

# promote_data.sh — publish staged, validated feature CSVs from data/staging/
# into data/. This is the ONLY path (besides regenerate_data.sh --force) that
# writes to data/ directly — kept as a separate, explicit command so
# publishing real data is never an accidental side effect of generating or
# testing it (see regenerate_data.sh's header for what that cost once).
#
# Re-validates each file in staging immediately before copying (does not
# trust that regenerate_data.sh's earlier validation is still valid — the
# file on disk could have changed since) and refuses to promote ANY file if
# ANY file fails, so a partially-bad batch never silently overwrites data/.
#
# Usage:
#   scripts/promote_data.sh <TICKER> [<TICKER> ...]
#   scripts/promote_data.sh --all      # promote every features_*.csv in staging
#
# Example:
#   scripts/regenerate_data.sh data/01302020.NASDAQ_ITCH50 AAPL AMZN
#   scripts/promote_data.sh AAPL AMZN

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGING_DIR="$REPO_ROOT/data/staging"
DATA_DIR="$REPO_ROOT/data"

if [ $# -lt 1 ]; then
  echo "Usage: $0 <TICKER> [<TICKER> ...]" >&2
  echo "       $0 --all" >&2
  exit 1
fi

if [ ! -d "$STAGING_DIR" ]; then
  echo "Error: $STAGING_DIR does not exist — nothing has been staged. Run regenerate_data.sh first." >&2
  exit 1
fi

STAGED_FILES=()
if [ "${1:-}" = "--all" ]; then
  shopt -s nullglob
  for f in "$STAGING_DIR"/features_*.csv; do
    STAGED_FILES+=("$f")
  done
  shopt -u nullglob
  if [ ${#STAGED_FILES[@]} -eq 0 ]; then
    echo "Error: no features_*.csv found in $STAGING_DIR." >&2
    exit 1
  fi
else
  for TICKER in "$@"; do
    f="$STAGING_DIR/features_${TICKER}.csv"
    if [ ! -f "$f" ]; then
      echo "Error: $f not found in staging. Run regenerate_data.sh for $TICKER first." >&2
      exit 1
    fi
    STAGED_FILES+=("$f")
  done
fi

for f in "${STAGED_FILES[@]}"; do
  if [ ! -f "${f}.meta" ]; then
    echo "Error: $f has no provenance stamp (${f}.meta missing) — refusing to promote." >&2
    exit 1
  fi
done

echo "== Re-validating before promotion (not trusting a prior run's result) =="
python3 "$REPO_ROOT/scripts/validate_book.py" "${STAGED_FILES[@]}"
# Exits 1 on any failure; set -e stops this script here if so — nothing is
# copied to data/ unless every staged file passes right now.

echo ""
echo "== Promoting to $DATA_DIR =="
for f in "${STAGED_FILES[@]}"; do
  base="$(basename "$f")"
  cp "$f" "$DATA_DIR/$base"
  cp "${f}.meta" "$DATA_DIR/${base}.meta"
  echo "  $f -> $DATA_DIR/$base"
done

echo ""
echo "Promoted ${#STAGED_FILES[@]} file(s) to $DATA_DIR."
