#!/bin/sh
# check-text-size.sh - Binary-size regression gate for libwirelog (Issue #460)
#
# Computes .text section size of the built library, compares against
# the committed baseline in tests/baseline_size.txt, and fails if
# growth exceeds the 5120-byte budget (section 14, Issue #446).
#
# Usage: scripts/ci/check-text-size.sh <path-to-library>
# Example: scripts/ci/check-text-size.sh builddir/libwirelog.so
#
# To update the baseline after legitimate growth:
#   echo <new-size> > tests/baseline_size.txt

set -e

THRESHOLD=5120
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BASELINE_FILE="$REPO_ROOT/tests/baseline_size.txt"

die() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

if [ $# -lt 1 ]; then
    die "usage: $0 <path-to-library>"
fi

LIB="$1"

if [ ! -f "$LIB" ]; then
    die "library not found: $LIB"
fi

# Extract .text section size (platform-aware)
extract_text_size() {
    case "$(uname -s)" in
        Linux)
            size --format=sysv "$1" | awk '/^\.text[[:space:]]/ { print $2 }'
            ;;
        Darwin)
            size -m "$1" | awk '/Section __text:/ { print $3 }'
            ;;
        *)
            die "unsupported platform: $(uname -s)"
            ;;
    esac
}

TEXT_SIZE=$(extract_text_size "$LIB")

if [ -z "$TEXT_SIZE" ]; then
    die "could not extract .text section size from $LIB"
fi

# Validate extracted size is numeric
case "$TEXT_SIZE" in
    ''|*[!0-9]*)
        die "non-numeric .text size extracted: '$TEXT_SIZE'"
        ;;
esac

# Read baseline
if [ ! -f "$BASELINE_FILE" ]; then
    die "baseline file not found: $BASELINE_FILE"
fi

BASELINE=$(tr -d '[:space:]' < "$BASELINE_FILE")

case "$BASELINE" in
    ''|*[!0-9]*)
        die "invalid baseline value in $BASELINE_FILE: '$BASELINE'"
        ;;
esac

# Compute delta (positive = growth, negative = shrinkage)
DELTA=$((TEXT_SIZE - BASELINE))

printf '.text size:  %d bytes\n' "$TEXT_SIZE"
printf 'baseline:    %d bytes\n' "$BASELINE"
if [ "$DELTA" -ge 0 ]; then
    printf 'delta:       +%d bytes\n' "$DELTA"
else
    printf 'delta:       %d bytes\n' "$DELTA"
fi
printf 'threshold:   +%d bytes\n' "$THRESHOLD"

if [ "$DELTA" -gt "$THRESHOLD" ]; then
    printf '\nFAIL: .text growth (+%d) exceeds %d-byte budget\n' "$DELTA" "$THRESHOLD" >&2
    printf 'To update the baseline after a legitimate change:\n' >&2
    printf '  echo %d > tests/baseline_size.txt\n' "$TEXT_SIZE" >&2
    exit 1
fi

printf '\nPASS: .text delta within budget\n'
