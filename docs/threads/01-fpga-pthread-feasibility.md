# FPGA + Pthread Feasibility Analysis

**Document**: `docs/threads/01-fpga-pthread-feasibility.md`
**Date**: 2026-03-06
**Status**: Phase 2C (pure C11 columnar backend, FPGA deferred to Phase 4+)

---

## Executive Summary

**Question**: Can FPGA acceleration coexist with pthread-based threading in wirelog's columnar backend?

**Answer**: **YES, with architectural conditions.** Pthreads are necessary for CPU intra-operator parallelism but insufficient as the sole model for FPGA offload. A phased approach (Option D) resolves this tension: implement CPU parallelism with pthreads now (Phases A–B-lite), then introduce a work-queue abstraction (Phase B-full) when FPGA hardware becomes concrete, enabling seamless transition to FPGA backends (Phase C) via Arrow IPC.

---

## 1. Current State Analysis: Single-Threaded Constraints

The columnar backend (`wirelog/backend/columnar_nanoarrow.c`) is explicitly designed as single-threaded, with three key architectural barriers to concurrent execution:

### 1.1 Thread-Safety Declaration (line 237)

```c
/**
 * Thread safety: NOT thread-safe. Each worker thread must own its session.
 *
 * @see backend_dd.c:35-44 for the embedding pattern reference (wl_dd_session_t)
 * @see session.h:38-40 for canonical wl_session_t definition
 * @see exec_plan.h for wl_plan_t backend-agnostic plan types
 */
typedef struct {
    wl_session_t base;       /* MUST be first field (vtable dispatch)  */
    const wl_plan_t *plan;   /* borrowed, lifetime: caller             */
    col_rel_t **rels;        /* owned array of owned col_rel_t*        */
    uint32_t nrels;          /* current number of registered relations */
    uint32_t rel_cap;        /* allocated capacity of rels[]           */
    wl_on_delta_fn delta_cb; /* delta callback (NULL = disabled)       */
    /* ... */
} wl_col_session_t;
```

**Implication**: The session structure maintains mutable state (`rels`, `nrels`, `rel_cap`, `delta_cb`) without synchronization primitives. Multiple threads cannot share a single session; each worker must own its own instance.

### 1.2 Global State in CONSOLIDATE (line 918)

```c
/* --- CONSOLIDATE --------------------------------------------------------- */

/* Comparison for qsort: lexicographic int64 row order. */
static uint32_t g_consolidate_ncols; /* qsort context (single-threaded) */

static int
row_cmp(const void *a, const void *b)
{
    const int64_t *ra = (const int64_t *)a;
    const int64_t *rb = (const int64_t *)b;
    for (uint32_t c = 0; c < g_consolidate_ncols; c++) {
        if (ra[c] < rb[c])
            return -1;
        if (ra[c] > rb[c])
            return 1;
        /* ... */
    }
    return 0;
}
```

**Implication**: The global variable `g_consolidate_ncols` stores the column count for `qsort`'s comparison function. Standard `qsort` has no context parameter, forcing the use of globals. This is fundamentally incompatible with concurrent calls; threads cannot simultaneously sort different relations with different column counts. Deduplication (which CONSOLIDATE performs) is an inherently sequential bottleneck in the current design.

### 1.3 Explicit Worker Count Ignoring (line 1473)

```c
/**
 * @see wl_session_create in session.c for vtable dispatch context
 * @see wl_col_session_t memory layout documentation above
 */
static int
col_session_create(const wl_plan_t *plan, uint32_t num_workers,
                   wl_session_t **out)
{
    (void)num_workers; /* columnar backend is single-threaded */

    if (!plan || !out)
        return EINVAL;

    wl_col_session_t *sess
        = (wl_col_session_t *)calloc(1, sizeof(wl_col_session_t));
    /* ... */
}
```

**Implication**: The columnar backend's `col_session_create` function receives a `num_workers` parameter from the abstraction layer (`backend.h`), but explicitly discards it with `(void)num_workers`. The backend ignores any attempt to parallelize work; it unconditionally creates a single-threaded session.

---

## 2. Pthread Model: Shared-Memory Architecture

Pthreads operate on a **shared-memory concurrency model**:

- Multiple threads share a single address space
- Synchronization via mutexes, condition variables, atomic operations
- Communication through shared data structures (queues, arrays, etc.)
- Data races detected by ThreadSanitizer, Helgrind
- **Requirement**: High-speed shared memory (same CPU socket or cross-socket via coherency)

### 2.1 Viable Pthread Use Cases in wirelog

1. **Inter-relation parallelism within strata**: Evaluate independent relations `R1`, `R2`, `R3` concurrently within a single stratum using a thread pool.
   - Example: Polonius benchmark has ~50 relations; Phase 2B can evaluate disjoint relations in parallel
   - **Constraint**: Each relation's evaluation must be independent (no write-after-read hazards)

2. **Intra-operator parallelism (future)**: Partition large JOINs or CONSOLIDATEs by key range
   - Example: If `JOIN(R1, R2)` produces 10M rows, partition by first 20% of keys to worker 1, next 20% to worker 2, etc.
   - **Constraint**: Requires refactoring qsort to use `qsort_r` (POSIX) or custom merge-sort with context passing
   - **Constraint**: Output relation must be merged/ordered post-sort across workers

### 2.2 Pthread Limitations for FPGA Integration

While pthreads are ideal for CPU parallelism, they **cannot directly express FPGA data transfers**:

| Aspect | Pthreads (Shared Memory) | FPGA (DMA Transfer) |
|--------|--------------------------|---------------------|
| **Communication** | Shared CPU memory, atomic ops | PCIe/DMA bulk transfers |
| **Synchronization** | Mutexes, condition variables, atomic CAS | Interrupt handlers, completion rings |
| **Data Movement** | Read/write via pointers | Serialized Arrow IPC batches |
| **Latency** | Nanoseconds | Microseconds to milliseconds |
| **Granularity** | Instruction-level (or thread pool tasks) | Batch-level (columnar arrays) |

**Key mismatch**: Pthreads assume threads share memory coherently. FPGA integration requires explicit serialization (Arrow IPC), host-device DMA transfers, and interrupt-driven completion. These are orthogonal concerns; mapping pthreads onto FPGA hardware is inefficient and incorrect.

---

## 3. FPGA Architectural Approach

wirelog's planned FPGA path is documented in `ARCHITECTURE.md` (lines 55–58 and 140–146):

### 3.1 Abstracted Compute Kernels and Arrow IPC (lines 55–58)

```
[FPGA Path] (future)
    ├─ Abstracted compute kernels
    ├─ Hardware offload
    └─ Arrow IPC data transfer
```

Arrow IPC is the **serialization bridge** between the CPU columnar backend and FPGA hardware:

- **Batch formation**: Pack relation columns into Arrow RecordBatch format
- **Schema negotiation**: Both CPU and FPGA agree on column types (int64, strings, etc.)
- **Zero-copy where possible**: Memory map Arrow buffers into FPGA host-side address space
- **PCIe DMA transfer**: Host-to-device (input batches), device-to-host (result batches)

### 3.2 ComputeBackend Abstraction (lines 140–146)

```
FPGA acceleration (future):
  wirelog (C11 parser/optimizer)
      ↓
  ComputeBackend abstraction
      ↓
  [CPU executor] or [FPGA via Arrow IPC]
```

The `wl_compute_backend_t` vtable (defined in `backend.h` lines 85–107) is the integration point:

```c
typedef struct {
    const char *name;

    int (*session_create)(const wl_plan_t *plan, uint32_t num_workers,
                          wl_session_t **out);
    void (*session_destroy)(wl_session_t *session);

    int (*session_insert)(wl_session_t *session, const char *relation,
                          const int64_t *data, uint32_t num_rows,
                          uint32_t num_cols);

    int (*session_remove)(wl_session_t *session, const char *relation,
                          const int64_t *data, uint32_t num_rows,
                          uint32_t num_cols);

    int (*session_step)(wl_session_t *session);

    void (*session_set_delta_cb)(wl_session_t *session, wl_on_delta_fn callback,
                                 void *user_data);

    int (*session_snapshot)(wl_session_t *session, wl_on_tuple_fn callback,
                            void *user_data);
} wl_compute_backend_t;
```

**Phase C design** will introduce a new FPGA backend implementing this vtable, with `session_snapshot` triggering Arrow IPC serialization, DMA transfers, and kernel invocation.

---

## 4. Detailed Assessment: Why Threading and FPGA Require Phased Sequencing

### 4.1 Concurrent CPU Execution (Pthreads)

**Phase A–B-lite work**:
1. Eliminate global state (`g_consolidate_ncols`) via context-passing sort (`qsort_r` or custom merge-sort)
2. Refactor session to allow independent worker ownership (each thread owns its session copy or partition)
3. Implement thread pool for inter-relation parallelism (independent relations within a stratum)
4. Verify output byte-for-byte matches single-threaded baseline (correctness gate)

**Why now**: CPU parallelism within the shared-memory model is straightforward with standard pthreads and mature debugging tools (ThreadSanitizer, Helgrind, GDB).

### 4.2 FPGA Integration (Arrow IPC + DMA)

**Phase B-full–C work**:
1. Define `wl_work_queue_t` abstraction (wrapping the Phase B-lite pthread pool)
2. Implement Arrow IPC serialization for work items
3. Introduce FPGA backend: allocate host-device DMA buffers, submit batches via PCIe, poll/interrupt on completion
4. Integrate with work-queue callbacks (FPGA completion signals work-queue dispatcher)

**Why later**: FPGA hardware platform selection and availability are Phase 4+ decisions. Designing a work-queue abstraction now (without concrete FPGA requirements) optimizes for a future that may not materialize as envisioned.

### 4.3 The Sequencing Tension

- **Argument for Option B** (work-queue from day one): Design the abstraction once, use it for both CPU and FPGA
  - **Cost**: Introduces novel abstraction simultaneously with first concurrent code; compounds debugging difficulty ("first concurrent code is also most abstract" risk)
  - **Benefit**: Single, coherent parallelism model from the start

- **Argument for Option D** (phased approach): CPU parallelism now, abstraction later
  - **Cost**: Moderate refactoring when FPGA phase begins (1–2 weeks to wrap pthread pool in work-queue)
  - **Benefit**: Delivers CPU value soonest, uses proven primitives, team gains threading experience incrementally, no premature abstraction

**Recommendation**: Option D. Wrapping a mature pthread pool in a `wl_work_queue_t` interface is a straightforward refactor with low risk, far lower than attempting Option B's upfront work-queue design concurrent with first-ever concurrent code.

---

## 5. Arrow IPC Integration Path

Arrow IPC enables separation of concerns:

### 5.1 Data Format Bridge

```
CPU Backend (Phase B-lite: pthreads)
    ↓ (serialize)
Arrow RecordBatch
    ↓ (DMA transfer)
FPGA Memory (Phase C)
    ↓ (compute kernel)
Arrow RecordBatch (results)
    ↓ (DMA transfer)
CPU Memory
    ↓ (deserialize)
CPU Backend (merge results)
```

### 5.2 Work-Queue Interface (Phase B-full)

```c
/* Sketch for Phase B-full: abstraction wrapping CPU thread pool and FPGA backend */
typedef struct {
    int (*submit)(wl_work_queue_t *q, const wl_work_item_t *item);
    int (*wait)(wl_work_queue_t *q);       /* block until item complete */
    int (*drain)(wl_work_queue_t *q);      /* synchronous fallback (single-threaded) */
    void (*destroy)(wl_work_queue_t *q);
} wl_work_queue_t;

/* CPU backend (Phase B-lite): thread pool, no Arrow IPC */
/* CPU backend (Phase B-full): same thread pool, wrapped by work-queue */
/* FPGA backend (Phase C): DMA transfers, Arrow IPC serialization, completion callbacks */
```

---

## 6. Fixed-Point Iteration Loop: Parallelizable vs Sequential

Polonius benchmark exhibits up to **1,487 fixed-point iterations**. Analysis of parallelizability:

| Part | Parallelizable? | Reason |
|------|-----------------|--------|
| **Relation evaluation (JOIN, CONSOLIDATE, etc.)** | YES (inter-relation) | Independent relations within a stratum can be evaluated in parallel |
| **Stratum sequencing** | NO | Each stratum reads outputs of previous stratum; inherently sequential |
| **Fixed-point convergence check** | NO | Requires aggregate statistics across all relations; must be sequential |
| **Delta propagation** | PARTIAL (Phase 2B) | Semi-naive optimization can enable incremental, parallel delta evaluation (Phase 2B+ work) |

**Implication**: Pthreads can exploit inter-relation parallelism within each stratum but cannot parallelizes the outer fixed-point loop. FPGA offload (Phase C) can accelerate individual expensive operators (large JOINs, CONSOLIDATEs) within the loop, reducing per-iteration cost.

---

## 7. Viability Conclusion

### 7.1 Feasibility Rating

| Aspect | Rating | Evidence |
|--------|--------|----------|
| **Pthread CPU parallelism** | **YES** | Proven by POSIX standard; mature debugging tools; orthogonal to current single-threaded design after Phase A refactoring |
| **FPGA as separate backend** | **YES** | Arrow IPC is standard (Apache Arrow), widely supported; `wl_compute_backend_t` vtable cleanly separates backends |
| **Pthread + FPGA coexistence** | **YES (with conditions)** | Requires phased approach: pthreads now (Phases A–B-lite), work-queue abstraction when FPGA hardware arrives (Phase B-full), then FPGA integration (Phase C) |
| **Overall architecturally achievable** | **YES** | Phased sequencing mitigates complexity and premature abstraction risk |

### 7.2 Why Pthreads Are Necessary but Not Sufficient

- **Necessary**: CPU parallelism is a prerequisite for acceptable performance in Phases A–B. Single-threaded evaluation of large Datalog programs (Polonius: 1,487 iterations over many relations) is unacceptable.
- **Not sufficient**: Pthreads assume shared-memory coherency. FPGA offload requires explicit data serialization (Arrow IPC) and DMA transfers, which are orthogonal to pthread shared-memory semantics.

### 7.3 Phased Adoption Resolves the Tension

```
Phase A (Phase 2C → A)
  └─ Eliminate global state (g_consolidate_ncols, qsort context)
     Output: All tests pass (TC, Polonius, RBAC) with byte-identical results

Phase B-lite (Phase A → B-lite)
  └─ Pthread inter-relation parallelism (thread pool, session partitioning)
     Output: ThreadSanitizer zero data races; Polonius p99 latency improved

Phase B-full (Phase B-lite → B-full, triggered when FPGA hardware selected)
  └─ Work-queue abstraction (wraps pthread pool, enables pluggable backends)
     Output: CPU backend unchanged, FPGA backend ready for integration

Phase C (Phase B-full → C, gated on FPGA availability + Phase 3 completion)
  └─ FPGA backend (Arrow IPC, DMA, kernel integration)
     Output: FPGA offload of expensive operators (JOIN, CONSOLIDATE)
```

**Upgrade cost (B-lite to B-full)**: Estimated 1–2 weeks. The pthread pool becomes the CPU backend of the work-queue; no throwaway code.

---

## 8. Constraints and Dependencies

### 8.1 Phase A Prerequisites

- Evaluate `qsort_r` portability across target platforms (macOS, Linux, RTOS)
  - Alternative: custom merge-sort with context passing (higher LOC but more portable)
- Refactor CONSOLIDATE operator to pass column count as context, not global state
- Verify output byte-for-byte against single-threaded baseline for all test suites (TC, Polonius, RBAC)

### 8.2 Phase B-full Trigger

- FPGA hardware platform selected and available for testing
- Phase 3 (nanoarrow executor documentation and embedded optimization) complete

### 8.3 Phase C Trigger Gates (all four must be met)

1. FPGA hardware platform selected and available for testing
2. Phase 3 (nanoarrow executor) complete
3. Phase B-lite benchmarks demonstrate CPU parallelism insufficient for target use cases
4. Arrow IPC serialization feasibility validated via prototype (round-trip serialize/deserialize of a representative relation batch)

---

## 9. Risk Mitigation

### 9.1 "First Concurrent Code Is Also Most Abstract" (Option B Risk)

**Risk**: Introducing `wl_work_queue_t` abstraction simultaneously with first concurrent code compounds debugging difficulty.

**Mitigation (Option D)**: Phase B-lite uses proven pthread primitives with mature debugging tooling (ThreadSanitizer, Helgrind). The work-queue abstraction is deferred until FPGA requirements are concrete.

**Alternative mitigation**: Prototype `wl_work_queue_t` on a single benchmark (Polonius JOIN operator) before full adoption, validating abstraction design in isolation.

### 9.2 Session Ownership Ambiguity

**Risk**: Multiple threads may inadvertently share a session, leading to data races.

**Mitigation**: Phase B-lite must enforce thread-local session ownership (each worker thread owns its session copy or partition). Document this invariant in `wl_col_session_t` comments. Use ThreadSanitizer CI to detect violations.

### 9.3 FPGA Hardware Lock-In

**Risk**: Arrow IPC integration may be difficult or inefficient for certain FPGA platforms.

**Mitigation**: Phase C design targets hardware-agnostic abstractions (OpenCL or HLS). Avoid Xilinx/Intel vendor-specific APIs. Prototype on open-source FPGA tools (Yosys, nextpnr) before committing to commercial platform.

---

## 10. Conclusion

**FPGA + pthread feasibility: YES, with architectural conditions.**

Pthreads are **necessary** for CPU intra-operator parallelism but **not sufficient** as the sole model for FPGA offload. Option D (phased approach) resolves this tension by delivering CPU parallelism value soonest (Phase B-lite) using proven, well-tooled primitives, while deferring the work-queue abstraction (Phase B-full) until FPGA hardware requirements are concrete.

The upgrade path from Phase B-lite to Phase B-full is low-cost (1–2 weeks to wrap the pthread pool), and no code is thrown away. The team gains threading experience incrementally before tackling abstraction design, minimizing risk and enabling informed FPGA integration decisions in Phase C.

**Recommendation**: Proceed with Option D phased approach. Phase A and B-lite are independent of FPGA decisions; Phase C remains gated on hardware availability and Phase 3 completion.
