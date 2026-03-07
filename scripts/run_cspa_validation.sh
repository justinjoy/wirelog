#!/usr/bin/env bash
# run_cspa_validation.sh - CSPA correctness and performance validation
#
# Usage:
#   ./scripts/run_cspa_validation.sh [--repeat N] [--build-dir DIR]
#
# Captures: wall time (min/median/max), peak RSS, output tuple count.
# Correctness gate: output tuples MUST equal BASELINE_TUPLES (20381).
# Performance gate: none (US-008 will set thresholds after US-007 passes).
#
# Exit codes:
#   0 - correctness PASS (tuple count matches baseline)
#   1 - build failure
#   2 - correctness FAIL (tuple count mismatch)
#   3 - benchmark binary not found

set -euo pipefail

# ----------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
BENCH_BIN="${BUILD_DIR}/bench/bench_flowlog"
DATA_CSPA="${REPO_ROOT}/bench/data/cspa"
RESULTS_DIR="${REPO_ROOT}/docs/performance"
RESULTS_FILE="${RESULTS_DIR}/cspa-validation-run-$(date +%Y%m%d-%H%M%S).tsv"

REPEAT="${REPEAT:-3}"
WORKERS="${WORKERS:-1}"

# Correctness oracle — must match baseline from benchmark-baseline-2026-03-07.md
BASELINE_TUPLES=20381

# ----------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeat)
            REPEAT="$2"; shift 2 ;;
        --build-dir)
            BUILD_DIR="$2"
            BENCH_BIN="${BUILD_DIR}/bench/bench_flowlog"
            shift 2 ;;
        --workers)
            WORKERS="$2"; shift 2 ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1 ;;
    esac
done

# ----------------------------------------------------------------
# Preflight checks
# ----------------------------------------------------------------

if [[ ! -f "${BENCH_BIN}" ]]; then
    echo "ERROR: bench binary not found: ${BENCH_BIN}" >&2
    echo "Run: meson compile -C build" >&2
    exit 3
fi

if [[ ! -d "${DATA_CSPA}" ]]; then
    echo "ERROR: CSPA data directory not found: ${DATA_CSPA}" >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"

# ----------------------------------------------------------------
# Collect system info
# ----------------------------------------------------------------

HOSTNAME_STR="$(hostname -s 2>/dev/null || echo unknown)"
ARCH_STR="$(uname -m 2>/dev/null || echo unknown)"
OS_STR="$(uname -sr 2>/dev/null || echo unknown)"
GIT_SHA="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_BRANCH="$(git -C "${REPO_ROOT}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"

echo "================================================================"
echo "CSPA Validation Run"
echo "================================================================"
echo "Binary   : ${BENCH_BIN}"
echo "Data     : ${DATA_CSPA}"
echo "Repeat   : ${REPEAT}"
echo "Workers  : ${WORKERS}"
echo "Git      : ${GIT_BRANCH} @ ${GIT_SHA}"
echo "Host     : ${HOSTNAME_STR} (${ARCH_STR}) ${OS_STR}"
echo "Baseline : ${BASELINE_TUPLES} tuples"
echo "Output   : ${RESULTS_FILE}"
echo "================================================================"

# ----------------------------------------------------------------
# Write TSV header
# ----------------------------------------------------------------

{
    echo "# CSPA Validation Run"
    echo "# Date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "# Branch: ${GIT_BRANCH} @ ${GIT_SHA}"
    echo "# Host: ${HOSTNAME_STR} (${ARCH_STR}) ${OS_STR}"
    echo "# Baseline tuples: ${BASELINE_TUPLES}"
    echo "# Columns: workload | input_facts | workers | repeat | min_ms | median_ms | max_ms | peak_rss_kb | output_tuples | status"
} > "${RESULTS_FILE}"

# ----------------------------------------------------------------
# Run benchmark
# ----------------------------------------------------------------

echo ""
echo "Running CSPA benchmark (repeat=${REPEAT}, workers=${WORKERS})..."
echo ""

RAW_OUTPUT="$("${BENCH_BIN}" \
    --workload cspa \
    --data-cspa "${DATA_CSPA}" \
    --workers "${WORKERS}" \
    --repeat "${REPEAT}" \
    2>&1)"

echo "${RAW_OUTPUT}"

# Append TSV line(s) to results file
echo "${RAW_OUTPUT}" | grep -v '^#' | grep -v '^$' >> "${RESULTS_FILE}" || true

# ----------------------------------------------------------------
# Parse output and verify correctness
# ----------------------------------------------------------------

# bench_flowlog TSV columns: workload | nodes | input_facts | workers | repeat |
#                             min_ms | median_ms | max_ms | peak_rss_kb | output_tuples | status
# CSPA emits "-" for nodes (multi-relation).

CSPA_LINE="$(echo "${RAW_OUTPUT}" | grep '^cspa' || true)"

if [[ -z "${CSPA_LINE}" ]]; then
    echo "" >&2
    echo "ERROR: No 'cspa' output line found in benchmark output." >&2
    exit 2
fi

# Extract fields (tab-separated)
TUPLES="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $10}')"
STATUS="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $11}')"
MIN_MS="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $6}')"
MEDIAN_MS="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $7}')"
MAX_MS="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $8}')"
PEAK_RSS="$(echo "${CSPA_LINE}" | awk -F'\t' '{print $9}')"

echo ""
echo "================================================================"
echo "Results Summary"
echo "================================================================"
echo "Status      : ${STATUS}"
echo "Tuples      : ${TUPLES}  (baseline: ${BASELINE_TUPLES})"
echo "Wall time   : min=${MIN_MS}ms  median=${MEDIAN_MS}ms  max=${MAX_MS}ms"
echo "Peak RSS    : ${PEAK_RSS} KB"
echo "================================================================"

# ----------------------------------------------------------------
# Correctness gate
# ----------------------------------------------------------------

if [[ "${STATUS}" != "OK" ]]; then
    echo ""
    echo "FAIL: Benchmark reported non-OK status: ${STATUS}"
    exit 2
fi

if [[ "${TUPLES}" != "${BASELINE_TUPLES}" ]]; then
    echo ""
    echo "CORRECTNESS FAIL: output tuples=${TUPLES}, expected=${BASELINE_TUPLES}"
    echo "This indicates a semantic regression in the CSE/delta-mode implementation."
    exit 2
fi

echo ""
echo "CORRECTNESS PASS: ${TUPLES} tuples == ${BASELINE_TUPLES} (baseline)"

# ----------------------------------------------------------------
# Append structured summary to results file
# ----------------------------------------------------------------

{
    echo ""
    echo "# --- Summary ---"
    echo "# correctness: PASS"
    echo "# tuples: ${TUPLES}"
    echo "# baseline: ${BASELINE_TUPLES}"
    echo "# min_ms: ${MIN_MS}"
    echo "# median_ms: ${MEDIAN_MS}"
    echo "# max_ms: ${MAX_MS}"
    echo "# peak_rss_kb: ${PEAK_RSS}"
} >> "${RESULTS_FILE}"

echo ""
echo "Results written to: ${RESULTS_FILE}"
exit 0
