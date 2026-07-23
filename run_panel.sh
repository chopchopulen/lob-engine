#!/usr/bin/env bash
# run_panel.sh — disk-safe one-at-a-time panel pipeline.
#
# For each date:
#   1. Download .gz
#   2. Decompress to scratch dir
#   3. Verify integrity
#   4. Run lob_engine for AAPL and AMZN
#   5. Save feature CSVs to data/
#   6. Delete .gz and decompressed binary
#   7. Move to next date
#
# Never keeps more than one raw ITCH file on disk at a time.
#
# Usage:
#   ./run_panel.sh                    # process all panel dates
#   ./run_panel.sh --skip-existing    # skip dates where both CSVs exist
#   ./run_panel.sh --dates 01302019,03282019

set -euo pipefail

ENGINE="./lob_engine"
BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
DATA_DIR="data"
SCRATCH_DIR="data/.scratch"
SKIP_EXISTING=false

ALL_DATES=(
    01302019 03272019 07302019 08302019 10302019 12302019
    01302020
)

TICKERS=(AAPL AMZN ETSY NFLX WDAY)

usage() {
    cat <<'EOF'
Usage: ./run_panel.sh [OPTIONS]

Options:
  --dates d1,d2,...   Comma-separated MMDDYYYY dates (default: all panel dates)
  --skip-existing     Skip if CSVs already exist for ALL tickers
  -h, --help          Show this message

Outputs:
  data/features_{AAPL,AMZN,ETSY,NFLX,WDAY}_MMDDYYYY.csv
EOF
    exit 0
}

DATES=("${ALL_DATES[@]}")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dates)
            IFS=',' read -ra DATES <<< "$2"
            shift 2
            ;;
        --skip-existing)
            SKIP_EXISTING=true
            shift
            ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

if [[ ! -x "$ENGINE" ]]; then
    echo "ERROR: $ENGINE not found or not executable." >&2
    echo "  Run 'make' (or 'make lob_engine') first." >&2
    exit 1
fi

mkdir -p "$DATA_DIR" "$SCRATCH_DIR"

PASS=0; FAIL=0; SKIP=0

for DATE in "${DATES[@]}"; do
    # Skip only if ALL ticker CSVs already exist for this date
    if $SKIP_EXISTING; then
        all_done=true
        for T in "${TICKERS[@]}"; do
            [[ -f "${DATA_DIR}/features_${T}_${DATE}.csv" ]] || { all_done=false; break; }
        done
        if $all_done; then
            echo "[$DATE] SKIP — all ticker CSVs already exist"
            ((SKIP++)) || true
            continue
        fi
    fi

    GZ_FILE="${SCRATCH_DIR}/${DATE}.NASDAQ_ITCH50.gz"
    BIN_FILE="${SCRATCH_DIR}/${DATE}.NASDAQ_ITCH50"

    echo ""
    echo "══════════════════════════════════════════"
    echo "[$DATE] Starting"

    # ── Cleanup any leftover decompressed binary (but keep partial .gz for resume) ──
    rm -f "$BIN_FILE"

    # ── Download (up to 3 attempts, fresh start each time) ──────────
    URL="${BASE_URL}/${DATE}.NASDAQ_ITCH50.gz"
    DOWNLOAD_OK=false
    for attempt in 1 2 3; do
        echo "[$DATE] Downloading (attempt $attempt/3)..."
        rm -f "$GZ_FILE"
        if curl -fL --silent --show-error --connect-timeout 60 --max-time 7200 -o "$GZ_FILE" "$URL"; then
            DOWNLOAD_OK=true
            break
        fi
        echo "[$DATE] Download attempt $attempt failed" >&2
        [[ $attempt -lt 3 ]] && sleep 60
    done
    if ! $DOWNLOAD_OK; then
        echo "[$DATE] ERROR: download failed after 3 attempts — skipping" >&2
        rm -f "$GZ_FILE"
        ((FAIL++)) || true
        continue
    fi
    echo "[$DATE] Downloaded: $(du -h "$GZ_FILE" | cut -f1)"

    # ── Verify integrity ────────────────────────────────────────────
    echo "[$DATE] Verifying .gz..."
    if ! gzip -t "$GZ_FILE" 2>/dev/null; then
        echo "[$DATE] ERROR: .gz file corrupt — skipping" >&2
        rm -f "$GZ_FILE"
        ((FAIL++)) || true
        continue
    fi

    # ── Decompress (streaming; delete .gz immediately) ───────────────
    echo "[$DATE] Decompressing..."
    gunzip -c "$GZ_FILE" > "$BIN_FILE"
    rm -f "$GZ_FILE"

    if [[ ! -s "$BIN_FILE" ]]; then
        echo "[$DATE] ERROR: decompression produced empty file — skipping" >&2
        rm -f "$BIN_FILE"
        ((FAIL++)) || true
        continue
    fi
    echo "[$DATE] Binary: $(du -h "$BIN_FILE" | cut -f1)"

    # ── Run engine ──────────────────────────────────────────────────
    ENGINE_OK=true

    for TICKER in "${TICKERS[@]}"; do
        OUT="${DATA_DIR}/features_${TICKER}_${DATE}.csv"
        echo "[$DATE] Running lob_engine ${TICKER}..."
        if ! "$ENGINE" "$BIN_FILE" "$TICKER" "$OUT" 2>&1; then
            echo "[$DATE] ERROR: lob_engine ${TICKER} failed" >&2
            ENGINE_OK=false
        fi
    done

    # ── Cleanup raw binary ──────────────────────────────────────────
    echo "[$DATE] Deleting raw binary..."
    rm -f "$BIN_FILE"

    if $ENGINE_OK; then
        summary=""
        for TICKER in "${TICKERS[@]}"; do
            OUT="${DATA_DIR}/features_${TICKER}_${DATE}.csv"
            rows=$(wc -l < "$OUT" 2>/dev/null || echo "?")
            summary+="${TICKER}=$((rows - 1)) "
        done
        echo "[$DATE] DONE — ${summary}"
        ((PASS++)) || true
    else
        ((FAIL++)) || true
    fi
done

echo ""
echo "══════════════════════════════════════════"
echo "Panel run complete."
echo "  Passed:  $PASS"
echo "  Failed:  $FAIL"
echo "  Skipped: $SKIP"
echo "  Feature CSVs in: $DATA_DIR/"
echo ""
echo "Next step: python scripts/build_panel.py"
