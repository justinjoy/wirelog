#!/usr/bin/env bash
# scripts/run_doop_validation.sh - Full DOOP benchmark validation (US-010)
#
# Copyright (C) CleverPlant
# Licensed under LGPL-3.0
#
# Runs the DOOP Java points-to analysis (zxing dataset, 83MB, 34 CSV files)
# using the bench_flowlog binary and captures wall time, peak RSS, and tuple
# count.  Reports PASS/FAIL based on whether DOOP completes without error.
#
# Usage:
#   scripts/run_doop_validation.sh [--build-dir DIR] [--workers N]
#                                  [--repeat N] [--baseline FILE]
#
# Options:
#   --build-dir DIR   Meson build directory (default: build)
#   --workers N       Worker thread count (default: 1)
#   --repeat N        Benchmark repeat count (default: 1)
#   --baseline FILE   TSV file with baseline metrics for comparison
#
# Environment:
#   DOOP_DATA_DIR     Override for DOOP CSV data directory
#                     (default: bench/data/doop relative to project root)
#
# Output:
#   TSV header + one result row printed to stdout.
#   Exit code 0 = DOOP completed successfully (PASS).
#   Exit code 1 = DOOP failed or binary not found.
#
# Example:
#   cd /path/to/wirelog
#   meson compile -C build bench_flowlog
#   scripts/run_doop_validation.sh --workers 1 --repeat 3

set -euo pipefail

# -------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-build}"
WORKERS="${WORKERS:-1}"
REPEAT="${REPEAT:-1}"
BASELINE_FILE=""

# -------------------------------------------------------------------------
# Argument parsing
# -------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --workers)
            WORKERS="$2"
            shift 2
            ;;
        --repeat)
            REPEAT="$2"
            shift 2
            ;;
        --baseline)
            BASELINE_FILE="$2"
            shift 2
            ;;
        -h|--help)
            head -40 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            exit 1
            ;;
    esac
done

# -------------------------------------------------------------------------
# Resolve paths
# -------------------------------------------------------------------------

BENCH_BIN="$PROJECT_ROOT/$BUILD_DIR/bench/bench_flowlog"
DOOP_DATA="${DOOP_DATA_DIR:-$PROJECT_ROOT/bench/data/doop}"

# -------------------------------------------------------------------------
# Pre-flight checks
# -------------------------------------------------------------------------

echo "=== DOOP Validation (US-010) ==="
echo "Project root : $PROJECT_ROOT"
echo "Build dir    : $BUILD_DIR"
echo "Workers      : $WORKERS"
echo "Repeat       : $REPEAT"
echo "DOOP data    : $DOOP_DATA"
echo ""

# Check bench binary
if [[ ! -x "$BENCH_BIN" ]]; then
    echo "ERROR: bench_flowlog not found at $BENCH_BIN"
    echo "Build it first:"
    echo "  meson compile -C $BUILD_DIR bench_flowlog"
    exit 1
fi

# Check DOOP data directory
if [[ ! -d "$DOOP_DATA" ]]; then
    echo "ERROR: DOOP data directory not found: $DOOP_DATA"
    echo "Set DOOP_DATA_DIR to the zxing CSV directory."
    exit 1
fi

# Count CSV files
CSV_COUNT=$(find "$DOOP_DATA" -maxdepth 1 -name "*.csv" | wc -l | tr -d ' ')
echo "DOOP CSV files found: $CSV_COUNT (expected: 34)"
if [[ "$CSV_COUNT" -lt 34 ]]; then
    echo "ERROR: expected 34 CSV files, found $CSV_COUNT"
    exit 1
fi

# Spot-check a key file
ACTPARAM="$DOOP_DATA/ActualParam.csv"
if [[ ! -s "$ACTPARAM" ]]; then
    echo "ERROR: ActualParam.csv is missing or empty"
    exit 1
fi

TOTAL_BYTES=$(du -sb "$DOOP_DATA" 2>/dev/null | awk '{print $1}' || \
              find "$DOOP_DATA" -maxdepth 1 -name "*.csv" -exec stat -f%z {} \; 2>/dev/null | \
              awk '{s+=$1} END{print s}')
TOTAL_MB=$(( TOTAL_BYTES / 1048576 ))
echo "Dataset size : ~${TOTAL_MB} MB"
echo ""

# -------------------------------------------------------------------------
# Run benchmark
# -------------------------------------------------------------------------

echo "Running DOOP benchmark..."
echo ""

# Print TSV header
"$BENCH_BIN" --workload doop --data-doop "$DOOP_DATA" \
             --workers "$WORKERS" --repeat "$REPEAT" \
             --print-header 2>/dev/null || true

# Run and capture output
BENCH_OUTPUT=$("$BENCH_BIN" \
    --workload doop \
    --data-doop "$DOOP_DATA" \
    --workers "$WORKERS" \
    --repeat "$REPEAT" \
    2>&1)

echo "$BENCH_OUTPUT"
echo ""

# -------------------------------------------------------------------------
# Parse results
# -------------------------------------------------------------------------

# Expected TSV columns (from bench_flowlog print_header):
# workload nodes edges workers repeat min_ms median_ms max_ms peak_rss_kb tuples status

STATUS=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $NF}')
MIN_MS=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $6}')
MEDIAN_MS=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $7}')
MAX_MS=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $8}')
PEAK_RSS_KB=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $9}')
TUPLES=$(echo "$BENCH_OUTPUT" | grep '^doop' | awk '{print $10}')

# -------------------------------------------------------------------------
# Report metrics
# -------------------------------------------------------------------------

echo "=== DOOP Validation Results ==="
echo ""
echo "Status      : ${STATUS:-UNKNOWN}"
echo "Wall time   : min=${MIN_MS:-?}ms  median=${MEDIAN_MS:-?}ms  max=${MAX_MS:-?}ms"
echo "Peak RSS    : ${PEAK_RSS_KB:-?} KB"
echo "Output tuples: ${TUPLES:-?}"
echo ""

# -------------------------------------------------------------------------
# Correctness check
# -------------------------------------------------------------------------

if [[ "${STATUS:-FAIL}" != "OK" ]]; then
    echo "RESULT: FAIL - DOOP did not complete successfully"
    echo ""
    echo "This means the evaluator timed out or produced an error on DOOP."
    echo "Option 2 + CSE must enable DOOP to complete (currently DNF)."
    exit 1
fi

echo "RESULT: PASS - DOOP completed successfully"
echo ""

# -------------------------------------------------------------------------
# Baseline comparison (optional)
# -------------------------------------------------------------------------

if [[ -n "$BASELINE_FILE" && -f "$BASELINE_FILE" ]]; then
    echo "=== Baseline Comparison ==="
    BASE_STATUS=$(grep '^doop' "$BASELINE_FILE" | awk '{print $NF}')
    BASE_MEDIAN=$(grep '^doop' "$BASELINE_FILE" | awk '{print $7}')
    BASE_TUPLES=$(grep '^doop' "$BASELINE_FILE" | awk '{print $10}')

    echo "Baseline status  : ${BASE_STATUS:-?}"
    echo "Baseline median  : ${BASE_MEDIAN:-?}ms"
    echo "Baseline tuples  : ${BASE_TUPLES:-?}"
    echo ""

    # Tuple correctness: must match baseline exactly
    if [[ -n "$BASE_TUPLES" && -n "$TUPLES" && "$BASE_TUPLES" != "?" ]]; then
        if [[ "$TUPLES" == "$BASE_TUPLES" ]]; then
            echo "Tuple count: MATCH ($TUPLES)"
        else
            echo "Tuple count: MISMATCH (got $TUPLES, expected $BASE_TUPLES)"
            echo "RESULT: FAIL - Output tuple count does not match baseline"
            exit 1
        fi
    fi

    # Performance: report speedup (informational, not a pass/fail gate)
    if [[ -n "$BASE_MEDIAN" && -n "$MEDIAN_MS" ]]; then
        echo "Performance: ${MEDIAN_MS}ms vs baseline ${BASE_MEDIAN}ms"
    fi
    echo ""
fi

# -------------------------------------------------------------------------
# Save results for future baseline comparison
# -------------------------------------------------------------------------

RESULTS_DIR="$PROJECT_ROOT/docs/performance"
if [[ -d "$RESULTS_DIR" ]]; then
    RESULT_FILE="$RESULTS_DIR/doop-validation-$(date +%Y-%m-%d).tsv"
    echo "workload	nodes	edges	workers	repeat	min_ms	median_ms	max_ms	peak_rss_kb	tuples	status" \
        > "$RESULT_FILE"
    echo "$BENCH_OUTPUT" | grep '^doop' >> "$RESULT_FILE"
    echo "Results saved to: $RESULT_FILE"
fi

echo "=== DOOP validation complete ==="
exit 0
