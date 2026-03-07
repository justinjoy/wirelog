# Threading Execution Models Analysis

**Project**: wirelog columnar backend (`wirelog/backend/columnar_nanoarrow.c`)
**Branch**: `next/pure-c11`
**Context**: Phase 2C -- pure C11 columnar backend, DD removed, single-threaded

---

## Assessment Principles

All threading models are evaluated against these five principles derived from the RALPLAN-DR:

| # | Principle | Definition |
|---|-----------|------------|
| P1 | **C11 Purity** | All threading must use C11 standard facilities (`stdatomic.h`, `threads.h`) or POSIX pthreads; no C++ or platform-specific extensions |
| P2 | **Backend Abstraction Preservation** | Threading must work within the existing `wl_compute_backend_t` vtable (`wirelog/backend.h:85-107`); no API-breaking changes |
| P3 | **Embedded Compatibility** | Threading model must degrade gracefully to single-threaded on bare-metal/RTOS targets (no mandatory OS dependency) |
| P4 | **FPGA Data Path Alignment** | Threading architecture must not conflict with Arrow IPC data transfer patterns required for future FPGA offload |
| P5 | **Incremental Adoption** | Multi-threading must be opt-in per stratum/operator, not all-or-nothing |

---

## Codebase Context

Before evaluating models, it is important to understand the current threading state:

- **No concurrent code exists**: `col_session_create()` (`columnar_nanoarrow.c:1470`) casts away `num_workers` with `(void)num_workers`
- **Global mutable state**: `g_consolidate_ncols` (`columnar_nanoarrow.c:918`) is a `static uint32_t` used as `qsort` context, annotated `/* single-threaded */`
- **Explicit thread-safety disclaimer**: "NOT thread-safe. Each worker thread must own its session." (`columnar_nanoarrow.c:237`)
- **`wl_compute_backend_t` vtable** (`backend.h:85-107`): accepts `num_workers` in `session_create` but no backend uses it
- **Performance hotspots**: fixed-point iteration (up to 1,487 iterations), sort-merge JOINs, qsort-based CONSOLIDATE dedup
- **FPGA**: architecturally planned for Phase 4+ via Arrow IPC; no hardware platform selected

---

## Option A: Pthreads Only

### Description

Thread pool using POSIX pthreads with work-stealing queues. Large JOINs and CONSOLIDATEs are partitioned across workers by key range. Synchronization uses `stdatomic.h` lock-free primitives for the work queue and `pthread_mutex_t`/`pthread_cond_t` for coarse-grained coordination.

### Principle Assessment

| Principle | Rating | Rationale |
|-----------|--------|-----------|
| P1: C11 Purity | **PASS** | POSIX pthreads are available on all target platforms; `stdatomic.h` provides lock-free primitives. No C++ or vendor extensions required. |
| P2: Backend Abstraction | **PASS** | Thread pool is internal to the columnar backend. `wl_compute_backend_t` vtable (`backend.h:85-107`) is unchanged; `num_workers` parameter already exists in `session_create`. |
| P3: Embedded Compat | **PASS** | Degrades to single-threaded by setting `num_workers=1` or skipping pool creation. Thread pool code compiles out with `#ifdef WL_HAS_PTHREADS`. |
| P4: FPGA Alignment | **FAIL** | Pthreads assume shared-memory parallelism. FPGA requires DMA bulk transfers over PCIe -- a fundamentally different execution model. Two separate parallelism models would need to coexist without a unifying abstraction. |
| P5: Incremental Adoption | **PASS** | Individual operators (JOIN, CONSOLIDATE) can be parallelized independently. Small relations (<10K rows) skip partitioning and run single-threaded. |

### Pros

1. **Mature, well-understood API** available on all POSIX systems with decades of production use
2. **Best debugging tooling**: ThreadSanitizer, Helgrind, and GDB all have first-class pthread support
3. **Fine-grained parallelism**: large relations can be partitioned by key range for sort-merge JOIN
4. **C11 `stdatomic.h`** provides portable lock-free primitives for work queue internals
5. **Low abstraction overhead**: no novel types or interfaces to design and validate

### Cons

1. **Global state blocks concurrency**: `g_consolidate_ncols` (`columnar_nanoarrow.c:918`) must be refactored before any threading is possible; `qsort` with global context is not thread-safe
2. **No FPGA mapping**: separate code path needed for FPGA offload; no shared abstraction between CPU threads and DMA transfers
3. **Thread pool overhead**: for small relations (<10K rows), pool dispatch + synchronization cost may exceed the parallelism benefit
4. **`qsort_r` portability**: thread-safe sorting requires `qsort_r` (BSD/GNU extension, not C11 standard) or a custom context-passing merge-sort

### Implementation Complexity

**Medium**. Requires Phase A global state elimination first. Thread pool implementation is ~500-800 LOC. Well-understood patterns exist.

### Upgrade Path

Wrapping a mature pthread pool in a `wl_work_queue_t` abstraction is a straightforward refactor estimated at 1-2 weeks. The pthread pool becomes the CPU backend of the work-queue; no throwaway code.

---

## Option B: Async Work-Queue from Day One

### Description

Abstract "compute task" queue where backends submit self-contained work items. A `wl_work_queue_t` interface provides `submit()`, `wait()`, and `drain()` operations. The CPU backend uses a pthread thread pool; a future FPGA backend uses DMA/PCIe transfers. Each work item is a columnar batch (Arrow-aligned), eliminating shared mutable state.

### Principle Assessment

| Principle | Rating | Rationale |
|-----------|--------|-----------|
| P1: C11 Purity | **PASS** | Work-queue interface is pure C11. CPU backend implementation uses pthreads + `stdatomic.h`. |
| P2: Backend Abstraction | **PASS** | Work-queue is internal to backend execution. `wl_compute_backend_t` vtable unchanged. |
| P3: Embedded Compat | **PASS** | Single-threaded fallback via synchronous queue drain (process work items inline without spawning threads). |
| P4: FPGA Alignment | **PASS** | Same task abstraction works for both CPU parallelism and FPGA offload. Each work item maps naturally to an Arrow IPC batch for DMA transfer. Async completion callbacks align with DMA completion interrupts. |
| P5: Incremental Adoption | **PARTIAL** | Work items can be submitted per-operator, but the `wl_work_queue_t` abstraction must be designed and stabilized before any operator can use it. All-or-nothing at the abstraction level, even if adoption per operator is incremental. |

### Pros

1. **Single parallelism model** for both CPU and FPGA -- no architectural divergence
2. **Natural Arrow IPC fit**: each work item is a self-contained columnar batch, aligning with FPGA data transfer patterns
3. **Async completion callbacks**: DMA completion interrupts map directly to the work-queue completion mechanism
4. **Future-proof**: designed for the end-state architecture from the start

### Cons

1. **First concurrent code is also the most abstract**: introduces a novel `wl_work_queue_t` abstraction simultaneously with the project's first-ever concurrent code, compounding complexity. Bugs may be in the abstraction layer, the threading logic, or both -- making root-cause analysis significantly harder.
2. **Higher upfront design cost**: task descriptor, completion mechanism, error propagation, and cancellation semantics all need design before any parallelism benefit is delivered
3. **Per-task overhead**: work-queue dispatch has higher overhead than raw pthreads for CPU-only workloads (task allocation, completion tracking, callback invocation)
4. **No existing reference**: zero concurrent code in the codebase means no patterns to build on; the abstraction must be designed from scratch
5. **FPGA requirements are speculative**: FPGA is Phase 4+ with no hardware platform selected. Designing the abstraction now optimizes for a future that may not materialize as envisioned (YAGNI risk)

### Implementation Complexity

**High**. Requires Phase A global state elimination plus design and implementation of the `wl_work_queue_t` interface (~1200-1800 LOC). Debugging instrumentation for the custom abstraction must be built alongside it, since ThreadSanitizer/Helgrind do not understand custom task semantics.

### Upgrade Path

Already at the end-state architecture. No further abstraction changes needed for FPGA integration; only the FPGA backend implementation within the existing work-queue interface.

---

## Option C: Event-Driven Reactor

### Description

Single-threaded event loop with non-blocking operator execution. Heavy operators (large JOINs) can optionally be farmed out to a thread pool. The event loop processes operator completions and schedules dependent operators, maintaining execution order without shared mutable state in the hot path.

### Principle Assessment

| Principle | Rating | Rationale |
|-----------|--------|-----------|
| P1: C11 Purity | **PARTIAL** | No standard C11 event loop facility exists. Requires either a third-party library (libuv, libev) or a custom implementation. Custom implementation is C11-pure but adds significant code. |
| P2: Backend Abstraction | **PASS** | Event loop is internal to the columnar backend. Vtable unchanged. |
| P3: Embedded Compat | **PASS** | Single-threaded event loop runs on bare-metal. Optional thread pool can be disabled. |
| P4: FPGA Alignment | **PARTIAL** | Event loop can wait on DMA completion file descriptors, but the reactor pattern adds complexity (epoll/kqueue dispatch for FPGA interrupts). Not a natural fit for batch compute workloads. |
| P5: Incremental Adoption | **PASS** | Operators register as event sources individually. Non-async operators run synchronously within the loop. |

### Pros

1. **Avoids shared-state issues**: single event-loop thread eliminates data races in the control path
2. **Good fit for streaming/incremental**: `session_step()` maps naturally to event-loop ticks
3. **Low overhead for small workloads**: no thread pool creation cost when relations are small

### Cons

1. **Poor CPU utilization for compute-bound work**: single-threaded bottleneck for large JOINs unless farming to a thread pool -- which reintroduces the threading complexity this model aims to avoid
2. **No standard C11 event loop**: requires either an external dependency (violates minimal-dependency goals) or a custom implementation (~800-1200 LOC for a portable reactor)
3. **Architectural mismatch**: Datalog fixed-point evaluation is batch-compute, not I/O-driven. Event-driven patterns are optimized for I/O multiplexing, not CPU-bound parallel computation.
4. **Overkill for batch mode**: the primary use case (full `session_snapshot` evaluation) does not benefit from event-driven scheduling

### Implementation Complexity

**Medium-High**. Custom event loop implementation is moderate, but integrating thread-pool farming for heavy operators approaches Option B complexity without the FPGA alignment benefit.

### Upgrade Path

Transitioning to FPGA requires adding DMA completion as an event source and implementing batch serialization. The reactor model does not simplify FPGA integration compared to a direct work-queue approach.

---

## Option D: Phased Approach (Recommended)

### Description

Temporal decoupling: deliver CPU parallelism immediately via proven pthreads (Phase B-lite), then introduce the `wl_work_queue_t` abstraction (Phase B-full) only when FPGA hardware requirements become concrete. This reaches the same end-state as Option B via a safer, incremental path.

**Phase structure**:
- **Phase A**: Eliminate global state (`g_consolidate_ncols`), make all operators reentrant
- **Phase B-lite**: Pthread inter-relation parallelism within strata (direct `pthread_create`/`join` or lightweight pool)
- **Phase B-full**: Define `wl_work_queue_t` wrapping the existing pthread pool (triggered by FPGA hardware availability)
- **Phase C**: FPGA backend integration via Arrow IPC + DMA through `wl_work_queue_t`

### Principle Assessment

| Principle | Rating | Rationale |
|-----------|--------|-----------|
| P1: C11 Purity | **PASS** | Phase B-lite uses pthreads + `stdatomic.h`. Phase B-full work-queue remains pure C11. No phase introduces non-standard dependencies. |
| P2: Backend Abstraction | **PASS** | `wl_compute_backend_t` vtable (`backend.h:85-107`) unchanged throughout all phases. Threading is internal to the columnar backend. |
| P3: Embedded Compat | **PASS** | Phase B-lite degrades to single-threaded (skip pool creation when `num_workers <= 1`). Phase B-full adds synchronous queue drain fallback. |
| P4: FPGA Alignment | **PASS** | Phase B-full and Phase C explicitly target Arrow IPC + DMA. No conflicting patterns introduced in earlier phases. Pthread pool API kept clean enough to wrap. |
| P5: Incremental Adoption | **PASS** | Each phase is independently deployable. Phase B-lite is opt-in per stratum. Phase B-full wraps existing code without changing operator-level threading. |

### Pros

1. **Delivers CPU parallelism value soonest** (Phase B-lite) with lowest risk -- no novel abstractions in the critical first step
2. **Proven debugging tooling**: ThreadSanitizer, Helgrind, and GDB have mature pthread support for Phase B-lite, when the team encounters concurrency bugs for the first time
3. **Preserves full Option B upgrade path**: wrapping the pthread pool in `wl_work_queue_t` is estimated at 1-2 weeks when FPGA requirements arrive
4. **Incremental learning curve**: team gains threading experience with familiar primitives (Phase B-lite) before designing the work-queue abstraction (Phase B-full)
5. **No throwaway code**: the pthread pool from Phase B-lite becomes the CPU backend of the work-queue in Phase B-full
6. **YAGNI-compliant**: defers work-queue abstraction cost until FPGA requirements are concrete, avoiding premature optimization for speculative Phase 4+ needs

### Cons

1. **Moderate refactoring at Phase B-full**: wrapping the pthread pool in `wl_work_queue_t` requires touching pool internals (1-2 weeks estimated effort)
2. **Two transition points**: B-lite to B-full and B-full to C, versus one transition in Option B (direct to FPGA)
3. **API discipline required**: pthread pool API in Phase B-lite must be kept clean enough to wrap later; sloppy internal APIs increase Phase B-full refactoring cost
4. **Delayed FPGA readiness**: if FPGA arrives sooner than expected, Phase B-full must be fast-tracked before FPGA integration can begin

### Implementation Complexity

**Low** (Phase B-lite) to **Medium** (Phase B-full). Phase B-lite is essentially Option A. Phase B-full adds the `wl_work_queue_t` interface on top of a proven, debugged thread pool -- significantly easier than designing the abstraction from scratch (Option B).

### Upgrade Path

Phase B-lite -> Phase B-full (1-2 week wrap) -> Phase C (FPGA backend via `wl_work_queue_t`). Each transition is bounded and well-defined.

---

## Consolidated Principle Assessment

| Principle | Option A | Option B | Option C | Option D |
|-----------|----------|----------|----------|----------|
| P1: C11 Purity | PASS | PASS | PARTIAL | PASS |
| P2: Backend Abstraction | PASS | PASS | PASS | PASS |
| P3: Embedded Compat | PASS | PASS | PASS | PASS |
| P4: FPGA Alignment | FAIL | PASS | PARTIAL | PASS |
| P5: Incremental Adoption | PASS | PARTIAL | PASS | PASS |
| **Total PASS** | **4/5** | **4/5** | **3/5** | **5/5** |

---

## Trade-Off Tensions

### Tension 1: Abstraction Cost Now vs Refactoring Cost Later

Option B pays the abstraction cost upfront: designing `wl_work_queue_t` before any threading code exists. This front-loads complexity but avoids later refactoring. Option D defers the abstraction cost, accepting a 1-2 week refactoring effort when FPGA requirements materialize. The trade-off hinges on FPGA timeline certainty:

- **If FPGA arrives within 6 months**: Option B's upfront investment may pay off by avoiding the Phase B-full transition
- **If FPGA is 18+ months out (current estimate)**: Option D avoids carrying unused abstraction complexity for over a year, during which requirements may shift

### Tension 2: Simplicity vs Architectural Coherence

Option A is the simplest to implement but creates architectural divergence: CPU parallelism via pthreads and FPGA parallelism via DMA would coexist as separate, unrelated systems. Option B provides architectural coherence (single parallelism model) but sacrifices simplicity by introducing the `wl_work_queue_t` abstraction alongside first-ever concurrent code.

Option D resolves this tension temporally: simplicity now (pthreads), coherence later (work-queue wrap). The key assumption is that wrapping a mature, debugged pthread pool is significantly easier than designing the abstraction from scratch -- validated by the estimate that Phase B-full is a 1-2 week refactor versus Option B's multi-week design effort.

### Tension 3: Debugging Confidence vs Design Purity

Option B produces a cleaner architecture but with harder-to-debug code: custom abstractions require custom debugging instrumentation. Option D prioritizes debugging confidence by using mature tooling (ThreadSanitizer, Helgrind) for the team's first concurrent code, then layering the abstraction on top of proven, race-free code. This is the "crawl before you walk" argument -- the cost is two transitions instead of one, but each transition is lower-risk.

---

## Recommendation

**Option D (Phased Approach)** is recommended for the following reasons:

1. **Only option with 5/5 principle alignment.** Options A and B each fail or partially fail one principle. Option C partially fails two. Option D passes all five by using temporal decoupling -- pthreads satisfy near-term principles, work-queue satisfies FPGA alignment when needed.

2. **Lowest risk for first concurrent code.** The wirelog codebase has zero threading experience. Introducing a novel abstraction (`wl_work_queue_t`) simultaneously with first-ever concurrent code compounds complexity and debugging difficulty (Option B's primary risk). Option D uses proven, well-tooled primitives first.

3. **YAGNI-compliant.** FPGA is Phase 4+ with no hardware platform selected. The work-queue abstraction optimizes for speculative requirements. Option D defers this cost until requirements are concrete, with a bounded upgrade path (1-2 weeks).

4. **No throwaway code.** The pthread pool from Phase B-lite becomes the CPU backend of the work-queue in Phase B-full. Every line of Phase B-lite code remains in production through Phase C.

5. **Same end-state as Option B.** Option D reaches the same architecture (work-queue with pluggable CPU + FPGA backends) via a safer path. The destination is identical; only the journey differs.

Option A is not invalidated -- it is Phase B-lite of Option D. Option B is not invalidated -- it remains the correct end-state. Option C is partially invalidated for compute-bound batch Datalog evaluation.

---

## References

- `wirelog/backend.h:85-107` -- `wl_compute_backend_t` vtable definition
- `wirelog/backend/columnar_nanoarrow.c:918` -- `g_consolidate_ncols` global state
- `wirelog/backend/columnar_nanoarrow.c:237` -- thread-safety disclaimer
- `wirelog/backend/columnar_nanoarrow.c:1470-1473` -- `col_session_create()` ignoring `num_workers`
- `wirelog/backend/columnar_nanoarrow.c:920-932` -- `row_cmp()` using global context
