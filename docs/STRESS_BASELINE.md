# Stress Harness Baseline Protocol (Issue #594)

This document covers the K-Fusion arena stress harness that
ships in `tests/test_stress_harness.c`: how it is wired into CI,
how to run the release-tier (W=8) variant locally, and the flake-
baseline protocol the repo uses to triage intermittent failures.

## CI tier topology

The same `test_stress_harness` binary is registered thirteen times in
`tests/meson.build` with different `env:` blocks and meson `suite:`
tags so a single configurable harness backs three CI tiers
without duplicating code.  (Twelve in the table below plus
`stress_harness_nested_asan` under the `asan` suite -- omitted from
this table because it ships separately from the W/R/strategy axes.)

| Test name | Suite | Workload | W | R | Where it runs |
|---|---|---|---|---|---|
| `stress_harness_w2` | (default) | freeze-cycle | 2 | 200 | every PR (`ci-pr.yml` -> `meson test`) |
| `stress_harness_apply_roundtrip_pr` | (default) | apply-roundtrip | 2 | 500 | every PR |
| `stress_harness_rotation_pr` | (default) | rotation-vtable (standard) | 2 | 50 | every PR |
| `stress_harness_rotation_pr_mvcc` | (default) | rotation-vtable (mvcc) | 2 | 50 | every PR |
| `stress_harness_w4` | `stress-nightly` | freeze-cycle | 4 | 500 | nightly (`perf-nightly.yml`, 04:00 UTC) |
| `stress_harness_apply_roundtrip_nightly` | `stress-nightly` | apply-roundtrip | 4 | 5000 | nightly |
| `stress_harness_rotation_nightly` | `stress-nightly` | rotation-vtable (standard) | 4 | 500 | nightly |
| `stress_harness_rotation_nightly_mvcc` | `stress-nightly` | rotation-vtable (mvcc) | 4 | 500 | nightly |
| `stress_harness_w8` | `stress-release` | freeze-cycle | 8 | 1000 | release-tier (manual, see below) |
| `stress_harness_apply_roundtrip_release` | `stress-release` | apply-roundtrip | 8 | 50000 | release-tier (manual) |
| `stress_harness_rotation_release` | `stress-release` | rotation-vtable (standard) | 8 | 1000 | release-tier (manual) |
| `stress_harness_rotation_release_mvcc` | `stress-release` | rotation-vtable (mvcc) | 8 | 1000 | release-tier (manual) |

The PR tier runs in the default suite, picked up by every
`meson test -C build` invocation in `ci-pr.yml`/`ci-main.yml`. The
nightly tier runs under TSan in `perf-nightly.yml`'s `stress` job.
The release tier has no GitHub workflow today; release engineers
invoke it manually before tagging.

## Workloads

### `freeze-cycle`

Verbatim lift of #582's freeze-cycle stress. Each cycle:

1. coordinator alloc()s a sentinel handle on `coord->compound_arena`,
2. coordinator freeze()s the arena,
3. submit `W` workers via `wl_workqueue_submit`; each worker does
   `wl_compound_arena_lookup` (must succeed) plus
   `wl_compound_arena_alloc` (must refuse while frozen),
4. wait_all,
5. coordinator unfreeze + `gc_epoch_boundary` advances the epoch.

`R` is the cycle count and is bounded by the arena's 4096 epoch cap;
the harness rejects `WL_STRESS_R >= max_epochs` with a hard FAIL.

### `apply-roundtrip`

Pre-rotation handles must remain valid post-apply (#594 acceptance
bullet). Sequence:

1. allocate `R` rows, each carrying a non-zero handle in column 0,
2. build a deterministic remap: `new_handle = old_handle XOR
   0xDEADBEEFCAFEBABE`,
3. call `wl_handle_remap_apply_columns` (#589) to rewrite the
   column in place,
4. assert every cell carries the expected post-apply handle.

`W` is accepted but ignored: `wl_handle_remap_apply_columns` is
single-mutator by contract; concurrent appliers would corrupt each
other's prefix. The W parameter stays in the harness signature for
CI-tier wiring uniformity. `R` is row count, only bounded by
available memory; release-tier exercises 50000 rows.

### `rotation-vtable`

Drives the #600 rotation strategy vtable
(`sess->rotation_ops->rotate_eval_arena` and
`sess->rotation_ops->gc_epoch_boundary`) under W/R stress. The
freeze-cycle and apply-roundtrip workloads call rotation/GC
primitives DIRECTLY (`wl_arena_reset`,
`wl_compound_arena_gc_epoch_boundary`); none exercise the indirect
function-pointer dispatch #600 introduced. This workload closes that
gap.

Per cycle:

1. allocate `K=64` handles in the compound arena's current epoch
   (oracle slice),
2. call `sess->rotation_ops->rotate_eval_arena(sess)` (vtable hook 1),
3. assert all `K` handles still resolve via
   `wl_compound_arena_lookup` with size-equality (the "all
   pre-rotation handles valid" acceptance bullet),
4. call `sess->rotation_ops->gc_epoch_boundary(sess)` (vtable hook 2);
   this closes the current epoch and the just-validated handles
   become unreachable -- by design (`compound_arena.c:332-344`).

The validity check sits between the two vtable calls because
`gc_epoch_boundary` clears the closed generation; that is GC behavior
owned by the arena, not the #596 vtable contract. Step (3) asserts
that `rotate_eval_arena` does not invalidate compound handles.

`W` is accepted but ignored (single-mutator: rotation hooks walk the
eval arena's bump pointer and the compound arena's per-epoch
generation table, neither concurrency-safe). The harness uses a
mock session (mirrors `nested-asan`'s `make_mock_session`) -- the
rotation hooks only touch `sess->eval_arena` and
`sess->compound_arena`, so a `calloc`'d `wl_col_session_t` with those
two fields plus `rotation_ops` is sufficient. No
parser/optimizer/plan link cost.

#### Strategy axis: `WIRELOG_ROTATION`

The rotation-vtable workload is the only one that honors the
`WIRELOG_ROTATION` environment variable, parsed at workload start
(unknown values hard-FAIL):

- `WIRELOG_ROTATION=standard` (default): selects
  `col_rotation_standard_ops`. Behavior matches the pre-#600 direct
  calls.
- `WIRELOG_ROTATION=mvcc`: selects `col_rotation_mvcc_ops`. The MVCC
  vtable is a placeholder today (per `rotation_mvcc.c`); behavior is
  identical to `standard` until the real MVCC implementation lands.

Both variants are registered at every CI tier because #596's contract
is the dispatch path itself, not the strategy semantics: the MVCC
placeholder must remain reachable through the function pointer under
stress. Coverage delta vs `tests/test_rotation_strategy.c` (#600's
correctness test): that file functional-tests selection
(default-is-standard, env-override-mvcc) and runs ONE rotate+gc
dispatch on a live session with no churn. The rotation-vtable
workload exercises the dispatch under R cycles of pre-rotation alloc
fan-out and per-handle post-rotate validity oracle.

`R` cap is 1500 (mirrors `WL_NESTED_ASAN_R_CAP`); higher values
hard-FAIL with a parseable diagnostic to keep epoch headroom under
the compound arena's 4096-epoch ceiling.

#### Baseline pass rate

100/100 PR-tier rotation-vtable runs pass on the baseline machine
(`stress_harness_rotation_pr` + `stress_harness_rotation_pr_mvcc`
combined). Healthy floor: any single CI failure is a real regression,
not a flake.

## Running release-tier locally

There is no `release.yml` workflow yet (deferred follow-up). To
gate a release manually:

```bash
meson setup build-stress-tsan -Db_sanitize=thread -Db_lundef=false \
    -Dthreads=posix -Dtests=true --buildtype=debug
meson compile -C build-stress-tsan
TSAN_OPTIONS='halt_on_error=1' \
    meson test -C build-stress-tsan --suite stress-release \
    --print-errorlogs --num-processes 1
```

All four release-tier entries must pass:
`stress_harness_w8` (freeze-cycle, 1000 cycles),
`stress_harness_apply_roundtrip_release` (apply-roundtrip, 50000
rows), and the two `stress_harness_rotation_release{,_mvcc}` entries
(rotation-vtable, 1000 cycles each, both strategy variants). Wall
time is sub-minute per entry on a typical x86_64 laptop under TSan;
the test() entries set `timeout: 300-600` as headroom.

## Flake baseline protocol

A "flake" is a TSan-clean test that fails non-deterministically
under stress instrumentation. The baseline distinguishes flakes
from real regressions.

### Capturing a baseline

After a stress test lands or its parameters change, capture a
baseline pass-rate over many runs to learn the natural failure
floor. From a clean working tree:

```bash
# 100-run baseline for the PR tier (cheap, sub-minute total).
N=100
fails=0
for i in $(seq 1 "$N"); do
    meson test -C build stress_harness_w2 \
        stress_harness_apply_roundtrip_pr >/dev/null 2>&1 \
        || fails=$((fails + 1))
done
echo "PR-tier baseline: $((N - fails)) / $N pass"
```

Replace the test-name list with `--suite stress-nightly` or
`--suite stress-release` for the heavier tiers.

A 100-run pass rate of:

- **100 / 100** -> healthy. Any single failure in CI is a real
  regression, not a flake.
- **99 / 100 - 95 / 100** -> the test has a tolerable flake floor.
  Document the rate inline at the top of the workload function in
  `test_stress_harness.c` and treat single CI failures as flakes
  for retry, but trigger investigation if two consecutive nightly
  runs fail.
- **< 95 / 100** -> the test is too flaky to gate on. Either
  reduce R (lower confidence) or fix the underlying race before
  re-enabling the gate.

### Triaging a CI failure against the baseline

When a CI run reports `stress_harness_*` FAIL:

1. Re-run the failed job once. Pass on retry + healthy baseline =
   flake; close-as-flaky.
2. Persistent failure across two consecutive runs OR a healthy
   baseline (100/100) PROVES it's a regression. Open a P1 issue
   referencing the SHA, attach `meson-logs/testlog.txt` and the
   relevant TSan/ASAN diagnostic, and bisect.

### Re-baselining

Re-run the baseline capture whenever:

- The harness's `R` defaults change.
- A new workload is added.
- A worker_fn or apply pass implementation changes (the upstream
  primitives in `wl_handle_remap_apply_columns`,
  `wl_compound_arena_lookup`, etc.).
- TSan / sanitizer toolchain version changes in CI image.

Record the new baseline pass-rate in the file header docstring of
`test_stress_harness.c` so future on-callers do not have to
re-derive it.

## Out of scope (#594 follow-up tracking)

- **Release-tier CI workflow**: no `release.yml` exists yet. The
  W=8 invocation runs manually per the script above. Adding an
  automated release pipeline is a separate (follow-up) issue.
- **Cross-arena rotation workload**: this harness exercises the
  *in-place* apply pass (#589 / #590). Cross-arena rotation needs
  the #550 Option C `wl_session_rotate` helper, which has not
  landed; a sibling workload will be added when #550-C ships.
- **Existing hardcoded canaries**: `test_compound_arena_freeze_
  cycle_stress.c` (#582), `test_gc_freeze_alloc_race.c` (#584),
  and `test_worker_borrow_w2_tsan.c` (#592) are intentionally
  retained un-refactored. They serve as fixed-W canaries; the
  parameterizable harness is in addition to, not in place of,
  them.
