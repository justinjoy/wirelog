#!/usr/bin/env bash
# Contract tests for run-memory-pressure.sh. These fixtures are intentionally
# small; they validate evidence classification, not Wirelog workload memory.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
runner=$root/scripts/ci/run-memory-pressure.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/wirelog-memory-pressure.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

run_success=$tmp/success
if ! "$runner" --mode success-required --budget-bytes 1048576 \
    --ceiling-bytes 268435456 --artifact-dir "$run_success" \
    --timeout-seconds 10 -- sh -c 'printf contract-ok'; then
    if grep -q '"disposition": "SKIP"' "$run_success/evidence.json" 2>/dev/null; then
        echo "SKIP: local memory enforcement unavailable"
        exit 77
    fi
    exit 1
fi
grep -q '"disposition": "PASS"' "$run_success/evidence.json"
grep -q '"artifact_sha256"' "$run_success/evidence.json"
grep -q '"command_argv"' "$run_success/evidence.json"
grep -Eq '"binary_sha256": "[0-9a-f]{64}"' "$run_success/evidence.json"
grep -q '"memory_swap_max"' "$run_success/evidence.json"
grep -Eq '"swap_policy": "(disabled|not-applicable)"' "$run_success/evidence.json"
grep -q 'contract-ok' "$run_success/stdout.log"

# A bare SIGKILL is ambiguous and must not be accepted as a resource error.
run_ambiguous=$tmp/ambiguous
if "$runner" --mode expected-resource-error --budget-bytes 1048576 \
    --ceiling-bytes 268435456 --artifact-dir "$run_ambiguous" \
    --timeout-seconds 10 -- sh -c 'kill -KILL $$'; then
    echo "FAIL: ambiguous SIGKILL was accepted as a resource error" >&2
    exit 1
fi
grep -q '"disposition": "FAIL"' "$run_ambiguous/evidence.json"
grep -q '"resource_failure_evidence": false' "$run_ambiguous/evidence.json"

# Timeout is distinct from both success and resource failure.
run_timeout=$tmp/timeout
if "$runner" --mode success-required --budget-bytes 1048576 \
    --ceiling-bytes 268435456 --artifact-dir "$run_timeout" \
    --timeout-seconds 1 -- sh -c 'sleep 3'; then
    echo "FAIL: timeout fixture unexpectedly passed" >&2
    exit 1
fi
grep -q '"timeout": true' "$run_timeout/evidence.json"
grep -q '"disposition": "FAIL"' "$run_timeout/evidence.json"

# A command that ignores TERM is eventually killed by timeout's kill-after
# path; that is still a timeout, not an ambiguous resource failure.
run_hard_timeout=$tmp/hard-timeout
if "$runner" --mode success-required --budget-bytes 1048576 \
    --ceiling-bytes 268435456 --artifact-dir "$run_hard_timeout" \
    --timeout-seconds 1 -- sh -c 'trap "" TERM; sleep 3'; then
    echo "FAIL: hard-timeout fixture unexpectedly passed" >&2
    exit 1
fi
grep -q '"timeout": true' "$run_hard_timeout/evidence.json"
grep -q '"disposition": "FAIL"' "$run_hard_timeout/evidence.json"

# Quotes and backslashes in argv must remain valid JSON strings.
run_quoted=$tmp/quoted
if ! "$runner" --mode success-required --budget-bytes 1048576 \
    --ceiling-bytes 268435456 --artifact-dir "$run_quoted" \
    --timeout-seconds 10 -- sh -c 'printf "%s" "quoted"'; then
    exit 1
fi
grep -q '"command_argv"' "$run_quoted/evidence.json"
grep -Fq 'printf \"%s\" \"quoted\"' "$run_quoted/evidence.json"

echo "PASS: memory-pressure runner contract"
