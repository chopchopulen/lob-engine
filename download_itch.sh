#!/usr/bin/env bash
# download_itch.sh — standalone ITCH downloader (one file at a time).
# Does NOT delete raw files. Use run_panel.sh for the disk-safe pipeline.
#
# Usage:
#   ./download_itch.sh <date1> [date2 ...]
#   Dates must be MMDDYYYY format (e.g. 01302020).
#
# Downloads to data/raw/. Skips dates already decompressed.

set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
RAW_DIR="data/raw"

if [[ $# -eq 0 ]]; then
    cat >&2 <<'EOF'
Usage: ./download_itch.sh <date1> [date2 ...]
  Date format: MMDDYYYY  (e.g. 01302020)

Suggested panel dates:
  Normal:    01302019 03282019 07302019 10302019 12302019
  High-vol:  03022020 03092020 03162020
  Low-vol:   08012019 08152019 09052019
  Earnings:  01292019 04302019 01282020 04302020
EOF
    exit 1
fi

mkdir -p "$RAW_DIR"

PASS=0; FAIL=0; SKIP=0

for DATE in "$@"; do
    GZ_FILE="${RAW_DIR}/${DATE}.NASDAQ_ITCH50.gz"
    BIN_FILE="${RAW_DIR}/${DATE}.NASDAQ_ITCH50"

    if [[ -f "$BIN_FILE" ]]; then
        echo "[$DATE] SKIP — already decompressed at $BIN_FILE"
        ((SKIP++)) || true
        continue
    fi

    echo ""
    echo "── $DATE ──────────────────────────────────"

    # Download
    if [[ ! -f "$GZ_FILE" ]]; then
        URL="${BASE_URL}/${DATE}.NASDAQ_ITCH50.gz"
        echo "[$DATE] Downloading $URL"
        if ! curl -fSL --progress-bar -o "$GZ_FILE" "$URL"; then
            echo "[$DATE] ERROR: download failed" >&2
            rm -f "$GZ_FILE"
            ((FAIL++)) || true
            continue
        fi
        echo "[$DATE] Downloaded: $(du -h "$GZ_FILE" | cut -f1)"
    else
        echo "[$DATE] .gz already present — skipping download"
    fi

    # Verify .gz integrity
    echo "[$DATE] Verifying .gz integrity..."
    if ! gzip -t "$GZ_FILE" 2>/dev/null; then
        echo "[$DATE] ERROR: .gz file corrupt" >&2
        rm -f "$GZ_FILE"
        ((FAIL++)) || true
        continue
    fi

    # Decompress (keep .gz for now; caller can delete)
    echo "[$DATE] Decompressing..."
    gunzip -c "$GZ_FILE" > "$BIN_FILE"

    if [[ ! -s "$BIN_FILE" ]]; then
        echo "[$DATE] ERROR: decompression produced empty file" >&2
        rm -f "$BIN_FILE"
        ((FAIL++)) || true
        continue
    fi

    echo "[$DATE] OK — binary: $(du -h "$BIN_FILE" | cut -f1)"
    ((PASS++)) || true
done

echo ""
echo "Download complete.  Passed: $PASS  Failed: $FAIL  Skipped: $SKIP"
echo "Files in: $RAW_DIR/"
