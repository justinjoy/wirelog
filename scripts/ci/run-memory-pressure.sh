#!/usr/bin/env bash
# Run one command under a real external memory ceiling and write evidence.
# This helper is qualification infrastructure; it does not claim that the
# command's allocations are admitted by Wirelog.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: run-memory-pressure.sh
  --mode success-required|expected-resource-error
  --budget-bytes BYTES
  --ceiling-bytes BYTES
  --artifact-dir DIR
  --timeout-seconds SECONDS
  [--allow-unsupported]
  -- COMMAND [ARGS...]
USAGE
}

mode=
budget_bytes=
ceiling_bytes=
artifact_dir=
timeout_seconds=
allow_unsupported=0

while (($#)); do
    case "$1" in
        --mode)
            (($# >= 2)) || { usage; exit 2; }
            mode=$2; shift 2 ;;
        --budget-bytes)
            (($# >= 2)) || { usage; exit 2; }
            budget_bytes=$2; shift 2 ;;
        --ceiling-bytes)
            (($# >= 2)) || { usage; exit 2; }
            ceiling_bytes=$2; shift 2 ;;
        --artifact-dir)
            (($# >= 2)) || { usage; exit 2; }
            artifact_dir=$2; shift 2 ;;
        --timeout-seconds)
            (($# >= 2)) || { usage; exit 2; }
            timeout_seconds=$2; shift 2 ;;
        --allow-unsupported)
            allow_unsupported=1; shift ;;
        --)
            shift; break ;;
        *)
            usage; exit 2 ;;
    esac
done

if [[ $mode != success-required && $mode != expected-resource-error ]] \
    || [[ ! $budget_bytes =~ ^[1-9][0-9]*$ ]] \
    || [[ ! $ceiling_bytes =~ ^[1-9][0-9]*$ ]] \
    || [[ ! $timeout_seconds =~ ^[1-9][0-9]*$ ]] \
    || [[ -z $artifact_dir || $# -eq 0 ]]; then
    usage
    exit 2
fi
if ((budget_bytes > ceiling_bytes)); then
    echo "FAIL: managed budget exceeds external ceiling" >&2
    exit 2
fi

mkdir -p "$artifact_dir"
stdout_file=$artifact_dir/stdout.log
stderr_file=$artifact_dir/stderr.log
evidence_file=$artifact_dir/evidence.json
timeout_marker=$artifact_dir/timeout.marker
: >"$stdout_file"
: >"$stderr_file"
: >"$timeout_marker"

json_escape() {
    local value=$1
    local escaped=
    local char
    local i
    for ((i = 0; i < ${#value}; i++)); do
        char=${value:i:1}
        case "$char" in
            $'\\') escaped="${escaped}\\\\" ;;
            '"') escaped="${escaped}\\\"" ;;
            $'\n') escaped="${escaped}\\n" ;;
            $'\r') escaped="${escaped}\\r" ;;
            $'\t') escaped="${escaped}\\t" ;;
            *) escaped+=$char ;;
        esac
    done
    printf '%s' "$escaped"
}

json_string() {
    printf '"%s"' "$(json_escape "$1")"
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        printf 'unavailable'
    fi
}

write_evidence() {
    local disposition=$1
    local enforcement=$2
    local status=$3
    local timeout_state=$4
    local resource_evidence=$5
    local cgroup_path=$6
    local memory_max=$7
    local memory_high=$8
    local memory_swap_max=$9
    local swap_policy=${10}
    local memory_peak=${11}
    local memory_events=${12}
    local command_json='['
    local arg
    local first=1

    for arg in "${command[@]}"; do
        if ((first)); then first=0; else command_json+=,; fi
        command_json+=$(json_string "$arg")
    done
    command_json+=']'

    local git_sha=unknown
    local git_dirty=false
    if git_sha=$(git rev-parse HEAD 2>/dev/null); then
        if [[ -n $(git status --porcelain 2>/dev/null) ]]; then
            git_dirty=true
        fi
    fi
    local script_sha
    script_sha=$(sha256_file "$0")
    local binary_sha=unavailable
    local binary_path=${command[0]}
    if [[ ! -f $binary_path ]]; then
        binary_path=$(command -v "${command[0]}" 2>/dev/null || true)
    fi
    if [[ -f $binary_path ]]; then
        binary_sha=$(sha256_file "$binary_path")
    fi
    local env_json
    env_json=$(env | awk -F= '/^(WIRELOG_MEMORY_BUDGET|WIRELOG_PERF_GATE|WIRELOG_PERF_REQUIRE|WIRELOG_TDD_MEMORY_BUDGET_BYTES)=/ {print}' \
        | while IFS= read -r line; do json_string "$line"; done \
        | paste -sd, -)
    [[ -n $env_json ]] || env_json=

    cat >"$evidence_file" <<EOF
{
  "schema": 1,
  "disposition": $(json_string "$disposition"),
  "mode": $(json_string "$mode"),
  "enforcement": $(json_string "$enforcement"),
  "managed_budget_bytes": $budget_bytes,
  "external_ceiling_bytes": $ceiling_bytes,
  "command_argv": $command_json,
  "environment_allowlist": [${env_json}],
  "source_commit": $(json_string "$git_sha"),
  "source_dirty": $git_dirty,
  "runner_sha256": $(json_string "$script_sha"),
  "binary_sha256": $(json_string "$binary_sha"),
  "artifact_sha256": {
    "stdout": $(json_string "$(sha256_file "$stdout_file")"),
    "stderr": $(json_string "$(sha256_file "$stderr_file")")
  },
  "cgroup_path": $(json_string "$cgroup_path"),
  "memory_max": $(json_string "$memory_max"),
  "memory_high": $(json_string "$memory_high"),
  "memory_swap_max": $(json_string "$memory_swap_max"),
  "swap_policy": $(json_string "$swap_policy"),
  "memory_peak": $(json_string "$memory_peak"),
  "memory_events": $(json_string "$memory_events"),
  "exit_status": $status,
  "timeout": $timeout_state,
  "resource_failure_evidence": $resource_evidence
}
EOF
}

command=("$@")
enforcement=
cgroup_path=
memory_max=unavailable
memory_high=unavailable
memory_swap_max=unavailable
swap_policy=unknown
memory_peak=unavailable
memory_events=unavailable
resource_evidence=false
status=0
timed_out=false

if [[ $(uname -s) != Linux ]] || ! command -v setsid >/dev/null 2>&1; then
    write_evidence SKIP unsupported 77 false false unavailable unavailable unavailable unavailable unknown unavailable unavailable
    echo "SKIP: Linux setsid/cgroup or RLIMIT enforcement is unavailable" >&2
    if ((allow_unsupported)); then exit 77; else exit 1; fi
fi

run_with_timeout() {
    local child
    local watchdog
    local child_status
    : >"$timeout_marker"
    setsid "$@" >"$stdout_file" 2>"$stderr_file" &
    child=$!
    (
        sleep "$timeout_seconds"
        if kill -0 "$child" 2>/dev/null; then
            printf 'true\n' >"$timeout_marker"
            kill -TERM -- "-$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null || true
            sleep 1
            if kill -0 "$child" 2>/dev/null; then
                kill -KILL -- "-$child" 2>/dev/null || kill -KILL "$child" 2>/dev/null || true
            fi
        fi
    ) &
    watchdog=$!
    if wait "$child"; then child_status=0; else child_status=$?; fi
    kill "$watchdog" 2>/dev/null || true
    wait "$watchdog" 2>/dev/null || true
    # The direct command may have left a background descendant in the same
    # process group after it returned.  Reap that group before reporting the
    # command status so cgroup cleanup and RLIMIT fallback share the same
    # lifecycle guarantee.
    kill -TERM -- "-$child" 2>/dev/null || true
    sleep 0.05
    kill -KILL -- "-$child" 2>/dev/null || true
    return "$child_status"
}

drain_cgroup_processes() {
    local pass
    local pid
    local pids
    for ((pass = 0; pass < 20; pass++)); do
        pids=$(cat "$cgroup_path/cgroup.procs" 2>/dev/null || true)
        [[ -z $pids ]] && return
        while IFS= read -r pid; do
            [[ -n $pid && $pid != $$ ]] || continue
            kill -TERM "$pid" 2>/dev/null || true
        done <<<"$pids"
        sleep 0.05
    done
    pids=$(cat "$cgroup_path/cgroup.procs" 2>/dev/null || true)
    while IFS= read -r pid; do
        [[ -n $pid && $pid != $$ ]] || continue
        kill -KILL "$pid" 2>/dev/null || true
    done <<<"$pids"
}

# Prefer a private cgroup v2 so the complete command tree is covered. A runner
# may deny creation even when cgroup v2 exists; RLIMIT_AS is a truthful
# kernel-enforced fallback, but its evidence is kept distinct from RSS.
cgroup_root=/sys/fs/cgroup
if [[ -f $cgroup_root/cgroup.controllers ]]; then
    candidate=$cgroup_root/wirelog-memory-$$
    if mkdir "$candidate" 2>/dev/null; then
        cleanup_candidate() { rmdir "$candidate" 2>/dev/null || true; }
        if printf '%s\n' "$ceiling_bytes" >"$candidate/memory.max" 2>/dev/null \
            && printf '0\n' >"$candidate/memory.swap.max" 2>/dev/null; then
            cgroup_path=$candidate
            enforcement=cgroup-v2
            swap_policy=disabled
            cleanup_cgroup() { rmdir "$cgroup_path" 2>/dev/null || true; }
            trap cleanup_cgroup EXIT
            (
                # Join before execing timeout so all of its descendants inherit
                # the private cgroup. The parent write closes the small startup
                # race for shells that schedule this block late.
                printf '%s\n' "$BASHPID" >"$cgroup_path/cgroup.procs" \
                    || exit 125
                run_with_timeout "${command[@]}"
            ) &
            child=$!
            if ! printf '%s\n' "$child" >"$cgroup_path/cgroup.procs" 2>/dev/null; then
                kill "$child" 2>/dev/null || true
                wait "$child" 2>/dev/null || true
                cleanup_cgroup
                trap - EXIT
                cgroup_path=
                enforcement=
            else
                if wait "$child"; then status=0; else status=$?; fi
            fi
            if [[ -n $cgroup_path ]]; then
                kill -TERM -- "-$child" 2>/dev/null || true
                sleep 0.05
                kill -KILL -- "-$child" 2>/dev/null || true
                drain_cgroup_processes
                memory_max=$(cat "$cgroup_path/memory.max" 2>/dev/null || printf unavailable)
                memory_high=$(cat "$cgroup_path/memory.high" 2>/dev/null || printf unavailable)
                memory_swap_max=$(cat "$cgroup_path/memory.swap.max" 2>/dev/null || printf unavailable)
                memory_peak=$(cat "$cgroup_path/memory.peak" 2>/dev/null || printf unavailable)
                memory_events=$(tr '\n' ';' <"$cgroup_path/memory.events" 2>/dev/null || printf unavailable)
                if grep -Eq '(^|;)oom_kill [1-9]|(^|;)oom [1-9]' <<<"$memory_events"; then
                    resource_evidence=true
                fi
            fi
        else
            cleanup_candidate
        fi
    fi
fi

if [[ -z $enforcement ]]; then
    if ! command -v prlimit >/dev/null 2>&1; then
        write_evidence SKIP unsupported 77 false false unavailable unavailable unavailable unavailable unknown unavailable unavailable
        echo "SKIP: no writable cgroup v2 and prlimit is unavailable" >&2
        if ((allow_unsupported)); then exit 77; else exit 1; fi
    fi
    enforcement=rlimit-as
    swap_policy=not-applicable
    if run_with_timeout prlimit --as="$ceiling_bytes" -- "${command[@]}"; then
        status=0
    else
        status=$?
    fi
fi

if [[ $(cat "$timeout_marker" 2>/dev/null) == true ]]; then
    timed_out=true
fi

disposition=FAIL
if [[ $mode == success-required && $status -eq 0 && $timed_out == false \
    && $resource_evidence == false ]]; then
    disposition=PASS
elif [[ $mode == expected-resource-error && $status -ne 0 \
    && $timed_out == false && $resource_evidence == true ]]; then
    disposition=EXPECTED_RESOURCE_ERROR
fi

write_evidence "$disposition" "$enforcement" "$status" "$timed_out" \
    "$resource_evidence" "${cgroup_path:-unavailable}" "$memory_max" \
    "$memory_high" "$memory_swap_max" "$swap_policy" "$memory_peak" \
    "$memory_events"
printf '%s: %s (evidence: %s)\n' "$disposition" "$enforcement" "$evidence_file"
[[ $disposition == PASS || $disposition == EXPECTED_RESOURCE_ERROR ]]
