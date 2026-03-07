# ADR-001: Workqueue Introduction Strategy

**Status**: Accepted
**Date**: 2026-03-06
**Reviewers**: Architect, Critic, Team Consensus

---

## Context

The wirelog columnar backend (`columnar_nanoarrow.c`) is currently single-threaded. The Polonius benchmark exhibits up to 1,487 fixed-point iterations over many relations. CPU parallelism is essential for acceptable performance. This ADR resolves the sequencing question: when and how to introduce the work-queue abstraction that will eventually support both CPU thread pools and FPGA offload.

## Decision

**Adopt Option B-lite: Introduce a minimal work-queue from day one.**

The work-queue will expose a 5-function interface (create, submit, wait_all, drain, destroy) with a pthread pool CPU backend (~300 LOC). This resolves the architectural tension between:
- **Option B** (full work-queue day one): Structurally isolates parallelism concerns but risks compounding complexity in first concurrent code
- **Option D** (phased pthreads then work-queue later): Delivers CPU parallelism soonest but requires a transition when FPGA requirements materialize

**Option B-lite synthesis**: Minimal work-queue that is thin enough for TSan/GDB tooling (mutexes + condvars only, no async callbacks) yet sufficient as the FPGA integration point. Captures Option B's structural isolation while preserving Option D's debugging confidence.

## Decision Drivers

1. **Risk profile of first concurrent code** (dominant): wirelog has zero existing threading. A minimal 300-LOC queue is debuggable with ThreadSanitizer and GDB. A full-featured Option B risks introducing abstraction bugs alongside threading bugs.

2. **Time to CPU parallelism value**: Option B-lite reaches parity with Option D (Phase B-lite) while establishing the correct abstraction from day one.

3. **Architectural coherence**: A single parallelism model (work-queue) from day one serves both CPU and FPGA backends, eliminating architectural divergence that Option D would require during Phase B-full transition.

## Alternatives Considered

| Option | Status | Rationale |
|--------|--------|-----------|
| A: Raw pthreads only | Invalidated | Creates architectural divergence: CPU parallelism via pthreads, future FPGA via separate code path. No unifying abstraction. Scores 1/5 on FPGA fit. |
| B: Full work-queue day one | Superseded by B-lite | 1200-1800 LOC estimate includes cancellation/error propagation/async callbacks. Minimal 300-LOC queue is viable without this overhead. |
| C: Event-driven reactor | Invalidated | Single-threaded event loop is fundamentally unsuited for compute-bound Datalog fixed-point iteration. Scores 1/5 on CPU parallelism ROI. |
| D: Phased pthread-first | Viable but superseded | Delivers CPU value soonest but defers work-queue until FPGA hardware selected (18+ months). B-lite achieves same CPU value while establishing correct abstraction. Requires Phase B-full transition. |

## Why B-lite

1. **Structural isolation**: Task isolation via work-queue prevents data races in result write-back path (see `shared-state-hazards.md`).
2. **Thin enough for tooling**: 300-LOC implementation using mutexes + condvars is fully TSan-visible and GDB-debuggable.
3. **Same end-state as Option B**: Architectural coherence, pluggable backends, no throwaway code.
4. **Avoids Phase B-full transition**: No refactoring when FPGA materializes — work-queue already exists.
5. **YAGNI-compliant**: Unlike full Option B's speculative design, minimal B-lite is scoped to immediate needs (thread pool) with extension points for FPGA.

## Consequences

### Architectural
- **wl_work_queue_t interface** becomes the canonical parallelism abstraction for the columnar backend
- **wl_compute_backend_t vtable** remains unchanged; threading is internal to backend implementation
- **FPGA backend** (Phase 4+) will implement work-queue interface with DMA/Arrow IPC serialization

### Implementation
- **Phase A**: Eliminate `g_consolidate_ncols` global state (1-2 weeks)
- **Phase B-lite+**: Implement `wl_work_queue_t` (300 LOC) + collect-then-merge parallelization (1-2 weeks)
- **Collect-then-merge mandate**: Each worker thread evaluates independent relations, collects results, main thread merges sequentially (structurally prevents hazards)
- **Per-worker arenas**: Each worker gets a clone of `eval_arena` to avoid allocator contention
- **Sort stability**: CONSOLIDATE output order declared unspecified (pragmatic, avoids stable-sort overhead)

### Testing & Verification
- **ThreadSanitizer CI mandatory**: All PRs touching `columnar_nanoarrow.c` must pass TSan (zero data races)
- **Output byte-for-bit match**: Single-threaded baseline (num_workers=1) vs multi-threaded (num_workers=4) — identical results
- **Benchmark non-regression**: Polonius p99 latency must not regress
- **Definition of Done**: All three gates pass before Phase B-lite is declared complete

## Completion Gates

Before Phase B-lite is complete:
- [ ] Phase A: Global state elimination (g_consolidate_ncols removed)
- [ ] TSan CI integration: All test suites pass with `-fsanitize=thread`, zero data race reports
- [ ] Output correctness: num_workers=4 output matches num_workers=1 baseline byte-for-bit
- [ ] Benchmark baseline: Polonius p99 latency captured with num_workers=1,2,4
- [ ] Work-queue API stable: 5-function interface (`create`, `submit`, `wait_all`, `drain`, `destroy`) finalized
- [ ] Collect-then-merge pattern validated: Single operator (CONSOLIDATE) tested with work-queue before full rollout

## Follow-ups

1. **Phase B-full trigger**: When FPGA hardware platform selected and available
   - Wrap existing work-queue CPU backend in platform-agnostic interface
   - Add async completion callback semantics for FPGA DMA events
   - No code changes to Phase B-lite; wrapping only

2. **Phase C**: FPGA backend integration (gated on 4 conditions: hardware + Phase 3 + CPU insufficient + Arrow IPC prototype)
   - Implement FPGA backend of `wl_work_queue_t` using DMA/Arrow IPC
   - Operator-level routing: offload JOIN/CONSOLIDATE if relation > 10K rows
   - Fall back to CPU for small relations

3. **Sort stability decision**: Before Phase A baseline, explicitly accept or reject the "output order unspecified" approach
   - If accepted: use `qsort_r` or custom sort, document unspecified ordering
   - If rejected: implement stable sort (custom merge sort, slightly higher LOC)

## Related Documents

- `docs/threads/01-fpga-pthread-feasibility.md` — Feasibility analysis
- `docs/threads/02-threading-models.md` — Detailed model evaluation (A, B, C, D)
- `docs/threads/03-tradeoff-matrix.md` — Quantified scoring (Option D wins both scenarios, but B-lite narrows gap)
- `docs/threads/04-implementation-roadmap.md` — Phased implementation plan
- `docs/threads/05-fpga-strategy.md` — FPGA acceleration strategy
- `docs/workqueue/shared-state-hazards.md` — Data race hazards in col_eval_stratum
- `docs/workqueue/api-sketch.md` — wl_work_queue_t interface and CPU backend design

---

## Consensus

✅ **Architect**: Approves B-lite synthesis as architecturally sound, recommends for its structural isolation and debuggability.

✅ **Critic**: Approves. All principles satisfied, alternatives fairly explored, risks identified and mitigated, acceptance criteria testable.

✅ **User**: Affirms preference for work-queue from day one; B-lite reconciles with team's threading expertise constraint.
