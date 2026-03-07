# Multi-Threaded Columnar Backend: Implementation Roadmap

**Document**: `docs/threads/04-implementation-roadmap.md`
**Date**: 2026-03-06
**Branch**: `next/pure-c11`
**Status**: Planning (pre-implementation)
**Depends on**: `02-threading-models.md`, `03-tradeoff-matrix.md`

---

## Overview

This roadmap describes the phased implementation of multi-threading in the wirelog columnar backend (Option D from `02-threading-models.md`). The four phases are designed to be independently deployable, with explicit correctness gates between each phase.

```
Phase A          Phase B-lite         Phase B-full               Phase C
Global State  →  Pthread Inter-    →  Work-Queue           →  FPGA Backend
Elimination      Relation Parallel    Abstraction             Integration
(1-2 weeks)      (3-4 weeks)          (1-2 weeks)             (T+18 months)
                                      [gate: FPGA hw]         [4 gates]
```

The codebase currently has zero concurrent execution. The columnar backend explicitly disables threading at three locations:

- `columnar_nanoarrow.c:1473`: `(void)num_workers; /* columnar backend is single-threaded */`
- `columnar_nanoarrow.c:237`: `/* Thread safety: NOT thread-safe. Each worker thread must own its session. */`
- `columnar_nanoarrow.c:918`: `static uint32_t g_consolidate_ncols; /* qsort context (single-threaded) */`

Phase A eliminates the single blocker that prevents even multiple independent sessions from safely parallelizing their own work. Phases B-lite through C add active intra-session and inter-backend parallelism.

---

## Phase A — Global State Elimination

**Estimated effort**: 1–2 weeks
**Status trigger**: Start immediately (no gates)

### Goal

Remove the only global mutable state in the columnar backend, making all operator functions reentrant. This is a prerequisite for any threading but also improves correctness guarantees even in single-threaded operation.

### Problem Statement

`columnar_nanoarrow.c:918`:
```c
static uint32_t g_consolidate_ncols; /* qsort context (single-threaded) */
```

The CONSOLIDATE operator (`col_op_consolidate`, line 935) must pass column-count context to `qsort`'s comparator function (`row_cmp`, line 921), which has a fixed signature `int cmp(const void *a, const void *b)`. The current workaround stores this context in a global variable, explicitly annotated as single-threaded.

Usage at `columnar_nanoarrow.c:965-966`:
```c
g_consolidate_ncols = nc;
qsort(work->data, nr, sizeof(int64_t) * nc, row_cmp);
```

If two threads invoke `col_op_consolidate` concurrently (even in separate sessions with separate stacks), the second write to `g_consolidate_ncols` races with the first's use in `row_cmp`.

### Approach: `qsort_r` or Context-Passing Merge Sort

Two implementation options, in order of preference:

**Option A1 — `qsort_r` (POSIX extension)**

`qsort_r` passes a caller-supplied context pointer to the comparator, eliminating the global:

```c
/* Prototype (Linux/glibc and POSIX.1-2024): */
void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *),
             void *arg);
```

The comparator becomes:
```c
static int row_cmp_r(const void *a, const void *b, void *ctx) {
    uint32_t ncols = *(const uint32_t *)ctx;
    /* ... same body as row_cmp ... */
}
```

**Portability note**: `qsort_r` is available on Linux (glibc ≥ 2.8), macOS (≥ 10.4, but with different argument order until macOS 12), and most BSDs. It is not part of C11 standard; availability must be checked via `_POSIX_C_SOURCE` or `__GLIBC__` guards. Embedded RTOS targets may not provide it.

**Option A2 — Custom stable merge sort with context parameter**

Implement a simple merge sort with signature `int wl_msort(void *base, size_t n, size_t width, int (*cmp)(const void *, const void *, void *), void *ctx)`. This is ~60–80 lines of C11, fully portable, and adds the benefit of **sort stability** (see Sort Stability note below).

**Recommendation**: Use `qsort_r` on platforms that provide it (Linux, macOS), falling back to a custom merge sort on embedded targets. Wrap the selection in `#ifdef` at compile time. This avoids a runtime branch and keeps the fast path using the OS-optimized sort.

### Sort Stability Note

`qsort` (and `qsort_r`) are not required to be stable. The CONSOLIDATE operator sorts rows for deduplication, and the dedup pass uses `memcmp` — so stability does not affect correctness of deduplication itself. However, output row ordering may differ between runs if equal rows exist (which they do, by definition, during deduplication). This can affect:

1. **Byte-for-bit reproducibility** across platforms when equal rows exist
2. **Downstream consumer expectations** if output order is implicitly relied upon

**Decision required**: Either (a) document that output order within a CONSOLIDATE result is unspecified, or (b) require a stable sort (custom merge sort) and guarantee stable output ordering. This decision must be made before Phase A correctness baseline is established, because the baseline defines what "correct" means for all subsequent phases.

### Affected Files

| File | Change |
|------|--------|
| `wirelog/backend/columnar_nanoarrow.c` | Remove `g_consolidate_ncols` (line 918); replace `row_cmp` (lines 921–932) with `row_cmp_r`; update `col_op_consolidate` (lines 935–983) to pass `&nc` to sort function |
| `wirelog/backend/columnar_nanoarrow.c` | Update `col_session_create` (line 1470) to remove `(void)num_workers` comment once sessions become threadable |

**Estimated LOC delta**: ~20 lines changed, 0 new files.

### Dependencies

- None. Phase A has no external dependencies and no blocking gates.
- Phase A does NOT require changes to `backend.h`, `session.h`, or `exec_plan.h`.
- The `wl_compute_backend_t` vtable (`backend.h:85–107`) is unchanged.

### Correctness Verification

**This is a refactor, not a feature addition. Output must be identical to pre-Phase-A.**

1. **Baseline capture** (before Phase A changes):
   - Run all test suites and capture output: TC, Polonius, RBAC
   - Record output tuples for each test as reference files (byte-for-bit snapshot)
   - Command: `meson test -C build --verbose 2>&1 | tee /tmp/phase-a-baseline.txt`

2. **Post-refactor validation**:
   - All test suites must pass with zero failures
   - Output tuples must match baseline byte-for-bit (or agree on stable ordering if stable sort chosen)
   - Compare: `diff /tmp/phase-a-baseline.txt /tmp/phase-a-post.txt`

3. **AddressSanitizer pass** (recommended alongside correctness):
   - Rebuild with `CC=clang CFLAGS="-fsanitize=address,undefined"` and re-run all tests
   - Zero ASan/UBSan errors required
   - Catches use-after-free, out-of-bounds, and integer overflow in the sort path

4. **No ThreadSanitizer in Phase A** (no concurrent execution yet; TSan mandatory from Phase B-lite onward)

### Phase A Completion Gate

Phase B-lite may not begin until:
- [ ] All test suites pass (TC, Polonius, RBAC)
- [ ] Output matches pre-Phase-A baseline byte-for-bit (or documented stable ordering)
- [ ] ASan+UBSan clean build passes all tests
- [ ] `g_consolidate_ncols` no longer exists in `columnar_nanoarrow.c`

---

## Phase B-lite — Pthread Inter-Relation Parallelism

**Estimated effort**: 3–4 weeks
**Status trigger**: Phase A completion gate passed

### Goal

Parallelize evaluation of independent relations within a stratum using a simple pthread pool. This targets the primary CPU performance bottleneck: the inner relation loop in `col_eval_stratum`, which evaluates each `wl_plan_relation_t` sequentially.

### Target Code

`col_eval_stratum` in `columnar_nanoarrow.c:1259` iterates relations sequentially:

```c
/* Non-recursive path (columnar_nanoarrow.c:1261–1327): */
if (!sp->is_recursive) {
    for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
        /* evaluate sp->relations[ri] ... */
    }
    return 0;
}
```

The `wl_plan_stratum_t` structure (`exec_plan.h:238–243`) provides:
```c
typedef struct {
    uint32_t stratum_id;
    bool is_recursive;
    const wl_plan_relation_t *relations;
    uint32_t relation_count;
} wl_plan_stratum_t;
```

For non-recursive strata, relations within a stratum that do not share IDB write targets are independent and can be evaluated in parallel. Each relation evaluation has its own `eval_stack_t` (local variable, stack-allocated) and reads from session relations (read-only during non-recursive evaluation).

**Recursive strata require sequential evaluation** (each iteration reads results written by the previous iteration for the same relation). Phase B-lite parallelizes non-recursive strata only; recursive stratum parallelism is deferred.

### Implementation

**No new abstractions.** Direct use of `pthread_create`/`pthread_join` or a minimal static thread pool (pre-created workers, condition-variable-based dispatch).

Design constraints:
- Thread pool is created once per session (`wl_col_session_t`) and destroyed with it
- 2–4 worker threads per session (configurable at session creation; `num_workers` parameter in `col_session_create` is currently ignored)
- Work granularity: one `wl_plan_relation_t` per work item
- Relations within a stratum that write to the same output relation must be serialized (append requires a mutex on the target relation)

**Affected areas**:

| File | Change |
|------|--------|
| `wirelog/backend/columnar_nanoarrow.c` | Add pthread pool lifecycle to `wl_col_session_t` (defined at line ~243); parallelize non-recursive inner loop in `col_eval_stratum` (line 1263); add mutex per output relation for concurrent append |
| `wirelog/session.h` | No API changes. `wl_session_create` signature (line 57–59) already accepts `num_workers`; `col_session_create` will consume it instead of casting to void |
| `wirelog/backend/columnar_nanoarrow.c` | Remove `(void)num_workers` at line 1473; use value to configure pool size |

**Estimated LOC delta**: ~150–200 lines added to `columnar_nanoarrow.c`, no new files.

The `wl_compute_backend_t` vtable (`backend.h:85–107`) is unchanged. The `num_workers` field is already part of the `session_create` function pointer signature (`backend.h:88`).

### Correctness Verification

**Correctness is verified independently of performance benchmarking.**

1. **Threaded output must match single-threaded baseline**:
   - Run all test suites with `num_workers=1` (single-threaded baseline) and `num_workers=4`
   - Output tuples must be identical (or agree on documented stable ordering)
   - Polonius benchmark output is the critical reference: thousands of derived tuples must match exactly

2. **ThreadSanitizer CI integration** (mandatory from Phase B-lite onward):
   - Add TSan build variant to CI: `CC=clang CFLAGS="-fsanitize=thread"`
   - All test suites must pass under TSan with zero data race reports
   - TSan is incompatible with ASan; run as separate CI jobs
   - TSan must run on every PR that touches `columnar_nanoarrow.c` or session lifecycle code

3. **AddressSanitizer** (recommended alongside TSan):
   - Separate CI job: `CFLAGS="-fsanitize=address,undefined"`
   - Catches heap corruption in concurrent append paths

4. **Helgrind (optional, supplementary)**:
   - `valgrind --tool=helgrind ./build/tests/test_columnar` on Polonius workload
   - Provides complementary data race detection with different false-positive profile than TSan

5. **Performance benchmarking** (separate activity, not a correctness gate):
   - Measure Polonius p99 latency with 1, 2, 4 workers
   - Target: measurable p99 improvement on multi-relation strata
   - Benchmark command: `./build/bench/bench_flowlog --workload all --data bench/data/graph_10.csv`
   - Performance results inform Phase C gate (c): whether CPU parallelism is sufficient

### Phase B-lite Completion Gate

Phase B-full may not begin until:
- [ ] All test suites pass with `num_workers=4` with output matching `num_workers=1` baseline
- [ ] TSan CI job passes with zero data race reports
- [ ] Polonius p99 latency benchmark captured and recorded (baseline for Phase C gate evaluation)

---

## Phase B-full — Work-Queue Abstraction Wrapper

**Estimated effort**: 1–2 weeks
**Status trigger**: FPGA hardware requirements become concrete (see gate below)

### Goal

Wrap the Phase B-lite pthread pool in a `wl_work_queue_t` internal interface, establishing a clean abstraction boundary between the CPU threading implementation and future FPGA DMA dispatch. This is a wrap, not a redesign — the pthread pool from Phase B-lite becomes the CPU backend of the work queue.

### Trigger Gate

**Phase B-full begins only when FPGA hardware requirements become concrete**, specifically when Phase C gate (a) is met:

> (a) FPGA hardware platform selected and available for testing

Before that gate is met, maintaining the direct pthread pool implementation in Phase B-lite is the correct approach. Introducing the abstraction prematurely adds complexity without delivering value (see `02-threading-models.md` "First concurrent code is also most abstract" risk analysis).

**If Phase C gate (a) is never met**, Phase B-full may be deferred indefinitely. The direct pthread pool from Phase B-lite is a fully acceptable production implementation.

### Implementation

Define a `wl_work_queue_t` internal interface (internal header, not part of public API):

```c
/* wirelog/backend/work_queue.h (new internal header) */
typedef struct wl_work_queue wl_work_queue_t;

typedef void (*wl_work_fn)(void *item, void *ctx);

wl_work_queue_t *wl_work_queue_create(uint32_t num_workers);
void             wl_work_queue_submit(wl_work_queue_t *q, wl_work_fn fn,
                                      void *item, void *ctx);
void             wl_work_queue_wait(wl_work_queue_t *q);
void             wl_work_queue_drain(wl_work_queue_t *q);  /* synchronous fallback */
void             wl_work_queue_destroy(wl_work_queue_t *q);
```

The CPU backend of `wl_work_queue_t` wraps the pthread pool from Phase B-lite. The single-threaded fallback calls `wl_work_queue_drain`, which executes submitted items synchronously on the caller thread — maintaining embedded compatibility without `#ifdef` in call sites.

**Single-benchmark validation before full rollout**:
- Validate `wl_work_queue_t` on the Polonius JOIN operator benchmark before replacing all Phase B-lite direct pthread calls
- Confirm zero performance regression and zero new TSan reports before broader adoption

**Affected files**:

| File | Change |
|------|--------|
| `wirelog/backend/work_queue.h` | New internal header (interface definition) |
| `wirelog/backend/work_queue.c` | New file (~100 lines): pthread pool wrapper implementation |
| `wirelog/backend/columnar_nanoarrow.c` | Replace direct `pthread_create`/`pthread_join` calls with `wl_work_queue_submit`/`wl_work_queue_wait` |

The `wl_compute_backend_t` vtable (`backend.h:85–107`) remains unchanged throughout Phase B-full.

### Correctness Verification

1. **Output must match Phase B-lite baseline**:
   - All test suites pass with output identical to Phase B-lite runs
   - Polonius output byte-for-bit match

2. **TSan CI pass**:
   - Zero data race reports (work queue introduces new synchronization points; TSan validates them)

3. **Single-benchmark gate**:
   - Polonius benchmark with `wl_work_queue_t` must match or exceed Phase B-lite performance
   - No regression allowed before replacing remaining direct pthread calls

---

## Phase C — FPGA Backend Integration

**Estimated effort**: Speculative; T+18 months from Phase A completion
**Status trigger**: All four gates below must be met simultaneously

### Trigger Gates (All Four Required)

Phase C does not begin until every gate is confirmed:

**(a) FPGA hardware platform selected and available for testing**
- Measurable criterion: specific vendor/board identified (e.g., AMD Alveo U250, Intel Agilex), development board physically available in test environment, PCIe or equivalent host interface operational
- Not met by: vendor selection alone without available hardware; simulation environments do not satisfy this gate

**(b) Phase 3 (nanoarrow executor) complete**
- Phase 3 refers to the nanoarrow executor documentation and embedded optimization work defined in `ARCHITECTURE.md` (lines 55–58)
- Measurable criterion: Phase 3 deliverables accepted and merged to main branch
- Rationale: FPGA data transfer via Arrow IPC requires the nanoarrow executor as the host-side serialization layer; FPGA integration without it requires duplicating Arrow IPC logic

**(c) CPU parallelism insufficient: Polonius p99 latency still unacceptable after Phase B-lite**
- Measurable criterion: Polonius p99 latency benchmark (captured in Phase B-lite completion) exceeds defined SLA with `num_workers=N` (N = available CPU cores)
- "Unacceptable" must be defined as a concrete latency threshold (e.g., "> 500ms p99 for standard Polonius workload") before this gate can be evaluated
- Not met by: theoretical reasoning alone; requires benchmark data from Phase B-lite

**(d) Arrow IPC serialization feasibility validated via prototype**
- Measurable criterion: working prototype that round-trip serializes and deserializes a representative `col_rel_t` batch (minimum: 10K rows, 4 columns) via Arrow IPC, with byte-for-bit result match and measured transfer overhead < target threshold
- The prototype must exercise the actual nanoarrow IPC path, not a mock
- Not met by: design documents or feasibility studies alone

### Approach

Phase C implements a new `wl_compute_backend_t` backend (`wl_backend_fpga`) that uses the `wl_work_queue_t` interface from Phase B-full to dispatch work items to FPGA kernels via Arrow IPC data transfer.

**Integration point**: `backend.h:85–107` — the `wl_compute_backend_t` vtable. Phase C adds a new vtable instance (e.g., `wl_backend_fpga()` analogous to the existing `wl_backend_columnar()` at `backend.h:117`). No changes to the vtable structure itself.

**Data transfer model**:
- Host-to-device: serialize `col_rel_t` batches as Arrow IPC record batches for FPGA kernel input
- Device-to-host: deserialize FPGA output as Arrow IPC record batches back to `col_rel_t`
- Work items in `wl_work_queue_t` correspond to Arrow IPC batch transfers to FPGA kernels

**FPGA-offloadable operators**: JOIN (sort-merge), CONSOLIDATE (sort + dedup), FILTER/MAP (element-wise). Fixed-point loop control, symbol interning, and stratum scheduling remain CPU-side.

See `05-fpga-strategy.md` for detailed operator offload analysis, DMA transfer patterns, and kernel interface design.

**Timeline**: T+18 months from Phase A completion is speculative and depends entirely on when all four gates are met. Phases B-full and Phase C may overlap once gate (a) is met.

### Correctness Verification

1. **FPGA output must match CPU columnar baseline**:
   - Run Polonius and TC workloads against both `wl_backend_columnar` and `wl_backend_fpga`
   - Output tuples must be identical
   - Arrow IPC round-trip must not introduce precision loss or reordering beyond documented sort stability policy

2. **TSan on host-side coordination code**:
   - FPGA DMA callbacks and completion handlers run in interrupt/driver context; host-side work queue coordination must be TSan-clean
   - Test with CPU-only mock FPGA backend (simulating async completion) before hardware integration

3. **ASan on Arrow IPC serialization path**:
   - The serialize/deserialize path in the prototype (gate d) must be ASan-clean before production use

---

## Cross-Phase Correctness Strategy

| Phase | Output Comparison | ThreadSanitizer | AddressSanitizer | Sort Stability |
|-------|------------------|-----------------|------------------|----------------|
| A (Global State Elimination) | vs. pre-A baseline (byte-for-bit) | Not applicable | Recommended | Decision required before baseline |
| B-lite (Pthread Parallelism) | vs. Phase A baseline (byte-for-bit) | **Mandatory** in CI | Recommended (separate job) | Inherited from Phase A decision |
| B-full (Work-Queue Wrapper) | vs. Phase B-lite baseline (byte-for-bit) | **Mandatory** in CI | Recommended | Unchanged |
| C (FPGA Backend) | vs. CPU columnar baseline (byte-for-bit) | Mandatory on host coordination | Mandatory on IPC path | Must match CPU policy |

**Key principle**: correctness verification (output matching + sanitizer passes) is a gate that must pass before declaring a phase complete. Performance benchmarking is a separate measurement activity that informs planning decisions but does not block correctness certification.

---

## Files Summary

| File | Phase A | Phase B-lite | Phase B-full | Phase C |
|------|---------|-------------|-------------|---------|
| `wirelog/backend/columnar_nanoarrow.c` | Remove `g_consolidate_ncols`; rewrite sort context | Add pthread pool; parallelize relation loop | Replace pthread calls with work queue | Minimal (FPGA backend is separate) |
| `wirelog/session.h` | None | None | None | None |
| `wirelog/backend.h` | None | None | None | Add `wl_backend_fpga()` declaration |
| `wirelog/backend/work_queue.h` | None | None | New (internal) | Used by FPGA backend |
| `wirelog/backend/work_queue.c` | None | None | New | Used by FPGA backend |
| `wirelog/backend/fpga_backend.c` | None | None | None | New (~300–500 LOC) |

---

## References

- `wirelog/backend/columnar_nanoarrow.c:918` — `g_consolidate_ncols` global variable (Phase A target)
- `wirelog/backend/columnar_nanoarrow.c:921–932` — `row_cmp` comparator using global context (Phase A target)
- `wirelog/backend/columnar_nanoarrow.c:237` — Thread safety annotation ("NOT thread-safe")
- `wirelog/backend/columnar_nanoarrow.c:1259` — `col_eval_stratum` (Phase B-lite target)
- `wirelog/backend/columnar_nanoarrow.c:1263` — Non-recursive relation loop (Phase B-lite parallelization point)
- `wirelog/backend/columnar_nanoarrow.c:1473` — `(void)num_workers` (Phase B-lite enables this)
- `wirelog/exec_plan.h:238–243` — `wl_plan_stratum_t` definition (relations array for Phase B-lite)
- `wirelog/backend.h:85–107` — `wl_compute_backend_t` vtable (Phase C integration point)
- `wirelog/backend.h:88` — `session_create` accepts `num_workers` (Phase B-lite activates)
- `docs/threads/02-threading-models.md` — Threading model analysis and Option D rationale
- `docs/threads/03-tradeoff-matrix.md` — Quantified trade-off scoring
- `docs/threads/05-fpga-strategy.md` — Phase C FPGA strategy detail
- `ARCHITECTURE.md:55–58` — Phase 3 (nanoarrow executor) as Phase C prerequisite
