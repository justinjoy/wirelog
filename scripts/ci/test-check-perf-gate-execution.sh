#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

write_log() {
    local path=$1
    local result=$2
    local name=$3
    local marker=$4
    cat >"$path" <<EOF
=================================== 1/1 ===================================
test:         perf - wirelog:${name}
result:       exit status ${result}
----------------------------------- stdout -----------------------------------
${marker}
==============================================================================
EOF
}

append_log() {
    local path=$1
    local result=$2
    local name=$3
    local marker=$4
    cat >>"$path" <<EOF
=================================== 1/1 ===================================
test:         perf - wirelog:${name}
result:       exit status ${result}
----------------------------------- stdout -----------------------------------
${marker}
==============================================================================
EOF
}

write_log "$tmp_dir/trace.log" 0 log_perf_gate 'test_log_perf_gate OK'
{
    write_log "$tmp_dir/error.log" 0 crdt_perf_gate 'test_crdt_perf_gate OK'
    append_log "$tmp_dir/error.log" 0 cspa_w1_gate 'test_cspa_perf_gate OK'
    append_log "$tmp_dir/error.log" 0 sub_ms_graph_perf_gate 'test_sub_ms_graph_perf_gate OK'
    append_log "$tmp_dir/error.log" 0 doop_w8_gate 'doop_w8_gate OK: tuples=13828835 iterations=153 median_ms=100 target_ms=105'
}

"$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/error.log"

write_log "$tmp_dir/bad-error.log" 77 crdt_perf_gate 'test_crdt_perf_gate: SKIP: governor unavailable'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/bad-error.log"; then
    echo "negative fixture unexpectedly passed" >&2
    exit 1
fi

write_log "$tmp_dir/cross-boundary.log" 0 crdt_perf_gate 'test_crdt_perf_gate: timing started'
append_log "$tmp_dir/cross-boundary.log" 0 cspa_w1_gate 'test_cspa_perf_gate OK'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/cross-boundary.log"; then
    echo "cross-boundary fixture unexpectedly passed" >&2
    exit 1
fi

append_log "$tmp_dir/duplicate.log" 0 crdt_perf_gate 'test_crdt_perf_gate OK'
append_log "$tmp_dir/duplicate.log" 0 crdt_perf_gate 'test_crdt_perf_gate OK'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/duplicate.log"; then
    echo "duplicate fixture unexpectedly passed" >&2
    exit 1
fi

cat >"$tmp_dir/incomplete.log" <<'EOF'
=================================== 1/1 ===================================
test:         perf - wirelog:crdt_perf_gate
result:       exit status 0
----------------------------------- stdout -----------------------------------
test_crdt_perf_gate OK
EOF
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/incomplete.log"; then
    echo "incomplete fixture unexpectedly passed" >&2
    exit 1
fi

write_log "$tmp_dir/correctness-only.log" 0 crdt_correctness_full 'test_crdt_perf_gate OK'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/correctness-only.log"; then
    echo "correctness-only fixture unexpectedly passed" >&2
    exit 1
fi

write_log "$tmp_dir/missing-doop.log" 0 crdt_perf_gate 'test_crdt_perf_gate OK'
append_log "$tmp_dir/missing-doop.log" 0 cspa_w1_gate 'test_cspa_perf_gate OK'
append_log "$tmp_dir/missing-doop.log" 0 sub_ms_graph_perf_gate 'test_sub_ms_graph_perf_gate OK'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/missing-doop.log"; then
    echo "missing DOOP fixture unexpectedly passed" >&2
    exit 1
fi

write_log "$tmp_dir/skipped-doop.log" 0 crdt_perf_gate 'test_crdt_perf_gate OK'
append_log "$tmp_dir/skipped-doop.log" 0 cspa_w1_gate 'test_cspa_perf_gate OK'
append_log "$tmp_dir/skipped-doop.log" 0 sub_ms_graph_perf_gate 'test_sub_ms_graph_perf_gate OK'
append_log "$tmp_dir/skipped-doop.log" 77 doop_w8_gate 'doop_w8_gate: SKIP: no calibrated 5-repetition W=8 target'
if "$script_dir/check-perf-gate-execution.sh" "$tmp_dir/trace.log" "$tmp_dir/skipped-doop.log"; then
    echo "skipped DOOP fixture unexpectedly passed" >&2
    exit 1
fi

echo "check-perf-gate-execution fixtures: OK"
