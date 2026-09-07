#!/usr/bin/env bash
# Deterministic fixture for the observable DOOP calibration contract.
set -euo pipefail

root=${1:-$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}
tmp=$(CDPATH= cd -- "$(mktemp -d "${TMPDIR:-/tmp}/wirelog-doop-validation.XXXXXX")" && pwd)
trap 'rm -rf "$tmp"' EXIT

fixture="$tmp/root"
mkdir -p "$fixture/scripts/release" "$fixture/build/bench" "$tmp/data" "$tmp/cgroup"
printf 'memory\n' > "$tmp/cgroup/cgroup.controllers"
: > "$tmp/cgroup/cgroup.procs"
cp "$root/scripts/run_doop_validation.sh" "$fixture/scripts/run_doop_validation.sh"
chmod +x "$fixture/scripts/run_doop_validation.sh"
for n in ActualParam $(seq 1 34); do
    printf 'fixture\n' > "$tmp/data/$n.facts"
done
facts_manifest=$(CDPATH= cd -- "$tmp/data" && sha256sum -- *.facts | LC_ALL=C sort -k2 | sha256sum | awk '{print $1}')
printf '%s\n' '# schema=2 workload tuple_oracle iteration_oracle data_path data_manifest_sha256 provenance_id acquisition_command' \
    > "$fixture/scripts/release/downstream-matrix-oracles.tsv"
printf 'doop\t7\t3\tbench/data/doop\tarchive:%064d;files:%s\tfixture\tfixture\n' 0 "$facts_manifest" \
    | sed 's/archive:0000000000000000000000000000000000000000000000000000000000000000/archive:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/' \
    >> "$fixture/scripts/release/downstream-matrix-oracles.tsv"

cat > "$fixture/build/bench/bench_flowlog" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
progress=
while (($#)); do
    if [[ $1 == --repeat-progress ]]; then progress=$2; shift 2; else shift; fi
done
mode=${DOOP_FIXTURE_MODE:-success}
if [[ -z "$(<"${WIRELOG_DOOP_CGROUP_DIR:?}/cgroup.procs")" ]]; then
    exit 91
fi
[[ "$(<"$WIRELOG_DOOP_CGROUP_DIR/memory.max")" == 1048576 ]] || exit 92
printf '%s\n' "${WIRELOG_DOOP_CGROUP_MARKER:?}" > "${WIRELOG_DOOP_CGROUP_MARKER}"
case "$mode" in
success)
    for r in 1 2 3 4 5; do
        printf 'repeat_start\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\n' "$r" >> "$progress"
        printf 'repeat_complete\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=0\tstatus=OK\ttuples=7\titerations=3\n' "$r" >> "$progress"
    done
    printf 'DONE\tworkload=doop\trepeat=5\tworkers=8\tstatus=OK\n' >> "$progress"
    printf 'doop\tfixture\t35\t8\t5\t1.0\t1.0\t1.0\t10\t7\t3\tOK\n'
    ;;
malformed|missing|nonsequential)
    printf 'repeat_start\tworkload=doop\trepetition=1\trepeat=5\tworkers=8\n' >> "$progress"
    if [[ $mode != missing ]]; then
        r=2; [[ $mode == nonsequential ]] && r=4
        printf 'repeat_complete\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=0\tstatus=OK\n' "$r" >> "$progress"
    fi
    printf 'doop\tfixture\t35\t8\t5\t1.0\t1.0\t1.0\t10\t7\t3\tOK\n'
    ;;
partial)
    printf 'repeat_start\tworkload=doop\trepetition=1\trepeat=5\tworkers=8\n' >> "$progress"
    printf 'repeat_complete\tworkload=doop\trepetition=1\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=0\tstatus=OK\n' >> "$progress"
    ;;
oom)
    printf 'oom 1\noom_kill 0\n' > "$WIRELOG_DOOP_CGROUP_DIR/memory.events"
    exit 0
    ;;
failed-repetition)
    printf 'repeat_start\tworkload=doop\trepetition=1\trepeat=5\tworkers=8\n' >> "$progress"
    printf 'repeat_complete\tworkload=doop\trepetition=1\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=1\tstatus=FAIL\tfailure_reason=fixture_failure\n' >> "$progress"
    exit 1
    ;;
timeout)
    (trap '' TERM; sleep 1000) & printf '%s\n' "$!" > "${DOOP_FIXTURE_CHILD:?}"
    sleep 1000
    ;;
early-leader)
    for r in 1 2 3 4 5; do
        printf 'repeat_start\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\n' "$r" >> "$progress"
        printf 'repeat_complete\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=0\tstatus=OK\ttuples=7\titerations=3\n' "$r" >> "$progress"
    done
    printf 'DONE\tworkload=doop\trepeat=5\tworkers=8\tstatus=OK\n' >> "$progress"
    printf 'doop\tfixture\t35\t8\t5\t1.0\t1.0\t1.0\t10\t7\t3\tOK\n'
    (trap '' TERM; sleep 1000) & printf '%s\n' "$!" > "${DOOP_FIXTURE_CHILD:?}"
    exit 0
    ;;
false-success)
    for r in 1 2 3 4 5; do
        printf 'repeat_start\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\n' "$r" >> "$progress"
        printf 'repeat_complete\tworkload=doop\trepetition=%s\trepeat=5\tworkers=8\tduration_ms=1.0\trss_kb=10\trc=0\tstatus=OK\n' "$r" >> "$progress"
    done
    printf 'DONE\tworkload=doop\trepeat=5\tworkers=8\tstatus=OK\n' >> "$progress"
    printf 'doop\tfixture\t35\t8\t5\t1.0\t1.0\t1.0\t10\t7\t3\tOK\n'
    exit 7
    ;;
esac
EOF
chmod +x "$fixture/build/bench/bench_flowlog"

run_case() {
    local name=$1 mode=$2 want=$3 wall_timeout=${4:-2}
    local evidence="$tmp/evidence-$name" output status
    set +e
    output=$(DOOP_FIXTURE_MODE="$mode" DOOP_DATA_DIR="$tmp/data" DOOP_CGROUP_ROOT="$tmp/cgroup" \
        DOOP_FIXTURE_CHILD="$tmp/child.pid" DOOP_CGROUP_MARKER="$evidence/cgroup-attached" \
        "$fixture/scripts/run_doop_validation.sh" --calibration \
        --memory-limit-kb 1024 --evidence-dir "$evidence" --wall-timeout "$wall_timeout" 2>&1)
    status=$?
    set -e
    [[ "$status" == "$want" ]] || {
        printf 'test-doop-validation: FAIL %s (want %s got %s)\n%s\n' \
            "$name" "$want" "$status" "$output" >&2
        find "$evidence" -type f -maxdepth 1 -print -exec sed 's/^/    /' {} \; 2>/dev/null || true
        return 1
    }
    printf 'test-doop-validation: ok %s\n' "$name"
    printf '%s' "$output" > "$tmp/$name.out"
}

run_case success success 0 10
grep -q 'RESULT: PASS' "$tmp/success.out"
[[ $(wc -l < "$tmp/evidence-success/repeat-progress.tsv") -eq 11 ]]
[[ -s "$tmp/evidence-success/final.tsv" ]]
[[ -s "$tmp/evidence-success/cgroup-attached" ]]
[[ -z "$(find "$tmp/cgroup" -mindepth 1 -maxdepth 1 -type d -name 'wirelog-doop-*' -print -quit)" ]]

printf 'doop\tfixture\t35\t8\t5\t1.0\t1.0\t1.0\t10\t999\t3\tOK\n' > "$tmp/baseline.tsv"
set +e
DOOP_FIXTURE_MODE=success DOOP_DATA_DIR="$tmp/data" DOOP_CGROUP_ROOT="$tmp/cgroup" \
    DOOP_FIXTURE_CHILD="$tmp/child.pid" DOOP_CGROUP_MARKER="$tmp/evidence-late/cgroup-attached" \
    "$fixture/scripts/run_doop_validation.sh" --calibration --memory-limit-kb 1024 \
    --baseline "$tmp/baseline.tsv" --evidence-dir "$tmp/evidence-late" --wall-timeout 10 > "$tmp/late.out" 2>&1
status=$?
set -e
[[ "$status" == 1 ]]
[[ ! -e "$tmp/evidence-late/final.tsv" ]]
[[ -z "$(compgen -G "$tmp/evidence-late/final.tsv.tmp.*" || true)" ]]
printf 'test-doop-validation: ok late_validation_failure\n'

set +e
DOOP_FIXTURE_MODE=success DOOP_DATA_DIR="$tmp/data" DOOP_CGROUP_ROOT="$tmp/missing-cgroup" \
    "$fixture/scripts/run_doop_validation.sh" --calibration --memory-limit-kb 1024 \
    --evidence-dir "$tmp/evidence-refused" --wall-timeout 2 > "$tmp/refused.out" 2>&1
status=$?
set -e
[[ "$status" == 1 ]]
grep -q 'memory_bound_refused' "$tmp/evidence-refused/metadata.tsv"
[[ ! -e "$tmp/evidence-refused/final.tsv" ]]
printf 'test-doop-validation: ok cgroup_refusal\n'

run_case malformed malformed 1
run_case missing missing 1
run_case nonsequential nonsequential 1
run_case partial partial 1
[[ -s "$tmp/evidence-partial/repeat-progress.tsv" ]]
[[ ! -e "$tmp/evidence-partial/final.tsv" ]]

run_case failed_repetition failed-repetition 1
grep -q $'status=FAIL\tfailure_reason=fixture_failure' \
    "$tmp/evidence-failed_repetition/repeat-progress.tsv"
[[ ! -e "$tmp/evidence-failed_repetition/final.tsv" ]]

run_case timeout timeout 1
grep -q $'calibration_failure\tworkload=doop\trepeat=5\tworkers=8\trc=124\tstatus=FAIL\tfailure_reason=timeout' \
    "$tmp/evidence-timeout/repeat-progress.tsv"
child=$(<"$tmp/child.pid")
! kill -0 "$child" 2>/dev/null

run_case early_leader early-leader 1
grep -q $'calibration_failure\tworkload=doop\trepeat=5\tworkers=8\trc=125\tstatus=FAIL\tfailure_reason=leader_exit' \
    "$tmp/evidence-early_leader/repeat-progress.tsv"
child=$(<"$tmp/child.pid")
! kill -0 "$child" 2>/dev/null

run_case oom oom 1
grep -q $'calibration_failure\tworkload=doop\trepeat=5\tworkers=8\trc=137\tstatus=FAIL\tfailure_reason=memory_oom' \
    "$tmp/evidence-oom/repeat-progress.tsv"
grep -q $'memory_events_oom_delta\t1' "$tmp/evidence-oom/metadata.tsv"

run_case false_success false-success 1
! grep -q 'RESULT: PASS' "$tmp/false_success.out"
[[ ! -e "$tmp/evidence-false_success/final.tsv" ]]

printf 'test-doop-validation: all cases passed\n'
