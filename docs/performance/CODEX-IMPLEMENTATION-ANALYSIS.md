# Codex Implementation Analysis

**Date:** 2026-03-08
**Role:** Implementation Specialist (Codex Review)
**Scope:** Feasibility, risk, and alternatives for K-fusion evaluator rewrite
**Verdict:** GO WITH CHANGES (see Final Recommendation)

---

## Executive Summary

The K-fusion strategy is architecturally sound but the planning documents contain significant internal contradictions and optimistic estimates that need correction before engineering begins. The core infrastructure (merge function, operator enum, worker stub) is already in place, which is good. However, the **critical missing piece is plan generation** -- and the documents dramatically underestimate the complexity of wiring K-fusion nodes into `expand_multiway_delta()` and the evaluator's interaction with the session layer. The biggest risk is not the merge algorithm (which is proven) but the **session/arena thread-safety gap**: workers currently call `col_eval_relation_plan()` which calls `col_op_variable()` which calls `session_find_rel()` -- a function that iterates an unprotected array. This is a data race waiting to happen and is not addressed anywhere in the design documents.

The 30-40% CSPA improvement target is likely **overestimated** for K=2. My analysis of the actual code suggests 15-25% is more realistic. The DOOP breakthrough (K=8) remains the high-value target and is where K-fusion has genuine potential. I recommend a CSPA-first sequencing with honest performance expectations, not parallel CSPA+DOOP.

---

## Feasibility Assessment

### Timeline Realism: OPTIMISTIC

The documents claim a 5-day critical path for core implementation. Here is a day-by-day reality check:

**Day 1 (Design): REALISTIC**
- Study existing infrastructure (2h) and design K-fusion integration (3h).
- This is already 80% done -- K-FUSION-DESIGN.md and K-FUSION-ARCHITECTURE.md exist.
- Actual remaining work: ~1 hour to finalize decisions.

**Day 2-3 (col_eval_relation_plan refactor, 6-8h estimate): OPTIMISTIC**
- The estimate of 6-8 hours for the refactor is aggressive. Here is what actually has to happen:
  1. Modify `expand_multiway_delta()` in exec_plan_gen.c to emit `WL_PLAN_OP_K_FUSION` nodes instead of K separate copies with CONCAT+CONSOLIDATE. This is not a simple conditional -- the current function produces `k * op_count + k + 1` ops in a flat array. Replacing this with a K-fusion node requires a **completely different output structure** since the K-fusion node must reference K sub-plans, not a flat op array. The current `wl_plan_op_t` struct has no field for sub-plan arrays (only `opaque_data` is mentioned in comments but doesn't exist in the struct).
  2. The `wl_plan_op_t` struct needs to be extended to carry K-fusion metadata. This is an ABI change to `exec_plan.h`, a shared header across backends. Adding a pointer field to a union-like flat struct requires careful thought.
  3. The worker function `col_op_k_fusion_worker()` calls `col_eval_relation_plan()` which calls `session_find_rel()` on a shared session. Multiple workers reading the same session concurrently is safe only if the session is truly read-only during worker execution. But `col_op_variable()` can push borrowed references to session-owned relations onto the eval stack, and downstream ops (JOIN, MAP) allocate new relations. If any worker triggers a `session_add_rel()` or realloc of session arrays, all concurrent reads become undefined behavior.
- **Realistic estimate: 12-16 hours** (2-3 full days, not 6-8 hours).

**Day 3 (CSPA wall-time measurement): UNREALISTIC**
- The plan says "Validate iteration count, overhead, root cause assumptions" by day 3.
- By day 3, the plan generation changes won't even be complete. You cannot measure wall-time on a feature that doesn't compile yet.
- **Realistic: Day 5-6 for first CSPA measurement.**

**Day 5 (DOOP validation): HIGHLY OPTIMISTIC**
- DOOP validation requires a fully working K-fusion path with K=8.
- K=8 means 8 workers, 8 arenas, 8 concurrent `col_eval_relation_plan()` calls.
- The K>=3 merge path uses **pairwise recursive merge** (line 2240-2266), which is O(K*M) instead of O(M log K). For K=8, this is 7 pairwise merges vs 1 heap merge. This needs optimization before DOOP can be validated.
- **Realistic: Day 8-10 for DOOP validation.**

### Top 3 Implementation Risks

**1. Session Thread-Safety (CRITICAL)**
- **Risk:** `col_op_k_fusion_worker()` calls `col_eval_relation_plan()` which calls `session_find_rel()` on a shared `wl_col_session_t*`. `session_find_rel()` iterates `sess->rels[]` without any lock. While the current design claims workers have "read-only session reference," this is only true if no worker triggers any session mutation. However, `col_op_variable()` with `WL_DELTA_FORCE_DELTA` falls back to the full relation (line 873) which is a session-owned pointer. If workers share a session and one worker's stack holds a borrowed `col_rel_t*` while another worker triggers a `session_add_rel()` that reallocs `sess->rels[]`, the borrowed pointer could be invalidated.
- **Mitigation:** Either (a) snapshot all relations needed by workers before dispatch (copy pointers into worker context, not the session pointer), or (b) give each worker a read-only session view with a frozen `rels[]` array. Option (a) is simpler but requires knowing which relations each sub-plan needs at dispatch time.
- **Severity:** Data corruption / segfault under concurrent execution.

**2. Plan Generation Structural Mismatch (HIGH)**
- **Risk:** The current plan is a flat `wl_plan_op_t[]` array per relation. K-fusion requires a hierarchical structure: a K-fusion node containing K sub-plans, each of which is itself a `wl_plan_op_t[]` array. The `wl_plan_op_t` struct (exec_plan.h) has **no field for sub-plan references**. The K-FUSION-DESIGN.md proposes a `col_op_k_fusion_t` struct with `wl_plan_op_t **ops`, but this type doesn't exist in the codebase, and the existing `wl_plan_op_t` has no union or pointer to carry it.
- **Mitigation:** Add an `opaque_data` void pointer to `wl_plan_op_t` and define `wl_plan_op_k_fusion_t` as a separate struct. This is a clean solution but modifies a shared ABI header. Alternatively, use a side-channel (session-level K-fusion metadata keyed by relation name).
- **Severity:** Blocks implementation until resolved. Not a runtime risk but a design gap.

**3. K-Way Merge Dedup Correctness for K>=3 (MEDIUM)**
- **Risk:** The current `col_rel_merge_k()` for K>=3 uses pairwise recursive merge (lines 2240-2266). This calls `col_rel_merge_k(pair, 2)` repeatedly, creating and freeing temporary relations. The dedup is correct per-pair but there is an edge case: if `relations[0]` has rows that duplicate with `relations[2]` but not `relations[1]`, the pairwise approach handles this correctly because the merged result of (0,1) is passed as input to merge with 2. However, the memory management is fragile:
  - Line 2251: `if (i > 1)` -- this means the original `relations[0]` is never freed by this code (it's not owned). But `temp` starts as `relations[0]` and is only freed when `i > 1`. If `relations[0]` is a session-owned relation, this is correct (don't free it). If it's a worker-allocated relation, it leaks.
  - Line 2259: The final `memcpy` into `out` followed by freeing `temp` is wasteful -- you could just return `temp` directly.
- **Mitigation:** For DOOP (K=8), replace pairwise merge with the proper min-heap merge from `col_op_consolidate_kway_merge()` (lines 1602-1689). This code already exists and is proven. The pairwise fallback should only be used as a reference, not in production.
- **Severity:** Memory leak for K>=3 with worker-allocated inputs. Performance degradation (O(K*M) vs O(M log K)).

---

## Code-Level Findings

### Per-Worker Arena: NEEDS REDESIGN

The K-FUSION-DESIGN.md proposes creating per-worker arenas:

```c
worker_arenas[i] = wl_arena_create(ARENA_CAPACITY);
```

However, examining the actual code, there is **no arena abstraction** in the columnar backend. The session uses `malloc`/`free` throughout (see `col_rel_new_auto()`, `col_rel_append_row()`, etc.). Relations are allocated with `malloc` and freed with `free`. There is no bump allocator or arena in the columnar backend.

The `wl_arena_t` type mentioned in design documents does not exist in the codebase. The workqueue.h header (line 44-62 per the design doc) does not contain any arena-related types -- it only declares the 5-function workqueue API.

**Assessment:** The "per-worker arena" concept from the design docs is **aspirational, not implementable** without first building an arena allocator. This is a significant hidden dependency. However, it may not be strictly necessary: if each worker allocates its own relations via `malloc` (which is thread-safe on all modern platforms), and workers never mutate shared state, the per-worker isolation can be achieved without arenas.

**Recommendation:** Drop the arena requirement. Use malloc-based allocation in workers (already thread-safe). Focus on ensuring workers don't access shared session state concurrently.

### K-Way Merge Correctness: SOUND WITH CAVEATS

Tracing through the K=2 merge (lines 2180-2238):

1. Two-pointer merge with `memcmp`-based comparison -- correct for sorted inputs.
2. Dedup via `last_row` tracking -- handles duplicates within and across streams.
3. Equal rows (cmp == 0): adds once, advances both pointers -- correct.

**Edge cases analyzed:**
- Empty left + non-empty right: outer loop skips, drain loop copies right -- correct.
- All duplicates across both streams: dedup emits one copy -- correct.
- Unsorted inputs: **incorrect results silently produced**. The merge assumes pre-sorted inputs but does not validate. If a worker produces unsorted output (e.g., because `col_op_consolidate` was not applied), the merge will produce garbage.

**Critical gap:** The K-fusion design eliminates the per-copy CONSOLIDATE step (that's the whole point), but the merge requires sorted inputs. Each worker's output must be sorted before merge. The design docs don't explicitly address when/where this sort happens. Looking at `col_eval_relation_plan()`, CONSOLIDATE ops in the plan do the sorting. If the plan generation removes the CONSOLIDATE from each K-copy (to avoid the redundant full-sort), the workers will produce unsorted output. If it keeps the CONSOLIDATE, each worker still does a full sort, and the savings come only from parallelism, not from eliminating sorts.

**This is the fundamental confusion in the design:** The bottleneck analysis says "eliminate 12 full sorts" but K-fusion as designed doesn't eliminate sorts -- it parallelizes them. The sorts still happen, just on different threads. The real savings are: (a) parallel execution of K joins, and (b) replacing K separate CONSOLIDATE+CONCAT+CONSOLIDATE with K parallel sorts + one K-way merge.

### Thread-Safety: UNSAFE AS DESIGNED

The worker function at line 2285-2290:
```c
col_op_k_fusion_worker(void *ctx) {
    col_op_k_fusion_worker_t *wc = (col_op_k_fusion_worker_t *)ctx;
    eval_stack_init(&wc->stack);
    wc->rc = col_eval_relation_plan(wc->plan, &wc->stack, wc->sess);
}
```

This passes `wc->sess` (the shared session) directly to `col_eval_relation_plan()`. Inside that function, every VARIABLE op calls `session_find_rel(sess, ...)` which reads `sess->rels[]` and `sess->nrels`. Multiple workers reading these fields concurrently is technically safe (no writes during worker execution, assuming the main thread is blocked on `wait_all()`). However:

1. `col_op_variable()` pushes **borrowed** pointers to session-owned relations onto the worker's eval stack. If the session is later modified (after barrier), these pointers become dangling. This is fine for the current design (results are extracted before session mutation), but fragile.

2. `col_op_join()` at line 1027 calls `session_find_rel(sess, op->right_relation)` -- multiple workers reading the same right relation concurrently is safe (read-only), but the join then iterates the right relation's data. If the right relation is being modified by another evaluator path (not in the K-fusion case, but in future), this would be a race.

**Assessment:** Thread-safety is **accidentally correct** for the current design (main thread blocked, session read-only during worker execution) but not **structurally guaranteed**. Any future change that allows session mutation during worker execution will introduce data races.

### Workqueue Overhead: LOW RISK WITH FALLBACK

The workqueue implementation is clean. Ring buffer with mutex+CV is standard. For K=2, the overhead is:
- 2 mutex lock/unlock pairs for submit (trivial)
- 1 CV signal per submit (trivial)
- 1 CV wait for barrier (trivial)
- Thread wake-up latency: ~10-50 microseconds on modern hardware

For relation evaluations taking milliseconds, this overhead is <1%. **The 5% threshold will not be exceeded** unless K=2 tasks are extremely fast (sub-millisecond), which only happens with tiny relations.

**Fallback:** `wl_workqueue_drain()` provides synchronous execution. If overhead is measured >5%, switch to drain mode for K=2, keep parallel for K>=4.

---

## Alternative Optimization Opportunities

### Beyond K-Fusion (Ranked by Effort/ROI)

1. **Incremental Consolidate in Semi-Naive Loop (already implemented!)**
   - `col_op_consolidate_incremental_delta()` at line 2031 already exists and is used in the semi-naive loop (line 2905). This is the "incremental consolidate" that Path A in the design docs claims needs to be built.
   - **Finding:** The bottleneck analysis may be using stale profiling data. The semi-naive loop already uses incremental consolidation with delta output. The claim that "full qsort on every iteration consumes 28-35% of wall time" needs re-validation against the current code.
   - **ROI:** Already captured. No additional work needed.

2. **Join Algorithm Optimization (HIGH ROI, MEDIUM EFFORT)**
   - The semijoin at line 2377-2387 uses a nested loop: O(left * right * keys). For large relations, this is the actual bottleneck, not consolidate.
   - Hash join or sort-merge join would reduce this to O(left + right).
   - **Estimated improvement:** 20-40% on join-heavy workloads (CSPA, DOOP).
   - **Effort:** 2-3 days for hash join implementation.
   - This is likely a **bigger win than K-fusion** for CSPA.

3. **CSE (Common Subexpression Elimination) Cache Optimization**
   - The `materialized` flag on ops (line 1021-1033) enables caching of shared join prefixes. If this isn't already exploited by the evaluator, enabling it could save redundant computation across K copies without parallelism.
   - **Effort:** 1 day to validate and tune.

4. **Empty-Delta Skip Enhancement**
   - `has_empty_forced_delta()` (line 2597-2624) already skips relations with empty forced deltas. This is a multiplicative optimization: if 50% of K-copies have empty deltas in later iterations, it halves the work without any parallelism.
   - **Finding:** This is already implemented. If delta expansion produces K=2 copies and one copy's forced-delta relation is empty, that entire copy is skipped. This means the actual K-copy overhead is less than the profiling claims (which assume all K copies execute every iteration).

### Streaming Evaluation (No Fixed-Point)

**Assessment: NOT APPLICABLE.** Wirelog implements Datalog semantics, which require fixed-point computation for recursive rules. Streaming evaluation (single-pass) is only valid for non-recursive strata, which are already evaluated once (line 2637-2704). There is no optimization opportunity here.

### DOOP-Specific Optimizations (K=8)

For DOOP with K=8, the following DOOP-specific optimizations should be considered:

1. **Reduce K via algebraic rewriting:** An 8-atom recursive body does not always need 8 delta copies. If some body atoms reference different IDB relations that are in different SCCs, K can be reduced. This is a plan-level optimization in `count_delta_positions()`.

2. **Batch worker submission:** For K=8, submit all 8 tasks at once, not one-by-one. The current workqueue supports this (ring buffer capacity 256 >> 8).

3. **Heap-based merge for K>=3:** Replace the pairwise fallback in `col_rel_merge_k()` with the heap-based merge from `col_op_consolidate_kway_merge()`. This reduces merge complexity from O(7*M) to O(M * log 8) = O(3M).

---

## Recommended Implementation Sequence

### Recommended: CSPA-First, DOOP-Second (Sequential)

**Rationale:** The documents propose parallel CSPA + DOOP validation. This is risky because:
- CSPA (K=2) is a simpler case that validates core correctness.
- DOOP (K=8) requires the K>=3 merge path, which has known issues (pairwise fallback, memory management).
- Debugging K=8 failures is much harder than K=2 failures.
- If K=2 doesn't show improvement, K=8 won't either (the per-copy overhead is the same).

**Proposed Sequence:**

| Phase | Days | Goal | Risk |
|-------|------|------|------|
| 1: Plan Gen + K=2 dispatch | 1-4 | K-fusion node in plan, K=2 dispatch working | Medium |
| 2: CSPA validation | 5-6 | All 15 workloads pass, measure wall-time | Low |
| 3: K>=3 heap merge | 7-8 | Replace pairwise with heap merge | Low |
| 4: DOOP validation | 9-10 | DOOP completes <5 min | Medium |
| 5: Optimization + polish | 11-15 | Hot-path tuning, stress tests | Low |

**Total: 3 weeks (15 working days)**

This is the same total duration as the current plan but with lower risk per phase and clear go/no-go gates:

- **Gate 1 (Day 4):** Does K=2 dispatch compile and pass unit tests? NO -> investigate plan gen structural issues.
- **Gate 2 (Day 6):** Does CSPA produce identical output? NO -> dedup bug in merge, rollback.
- **Gate 3 (Day 6):** Is CSPA wall-time improved? If <15% -> K-copy overhead was overestimated, pivot to join optimization.
- **Gate 4 (Day 10):** Does DOOP complete <5 min? NO -> investigate non-K-copy bottlenecks.

### Why NOT DOOP-First

The alternative of doing DOOP first (K=8) has higher reward if successful but:
- Requires the K>=3 merge path to be correct (currently uses untested pairwise fallback).
- Debugging 8-way concurrent execution is much harder.
- If DOOP fails, you don't know if it's K-fusion or another bottleneck.
- CSPA provides a controlled validation environment.

### Why NOT Parallel CSPA+DOOP

- Same engineer working on both creates context-switching overhead.
- Bugs found in DOOP may require changes to core merge/dispatch that invalidate CSPA results.
- Sequential approach allows learning from K=2 before tackling K=8.

---

## Testing Bottlenecks

### Critical by Day 4 (Must Have)

1. **K-fusion node creation in plan generation** -- unit test that `expand_multiway_delta()` produces `WL_PLAN_OP_K_FUSION` nodes with correct sub-plan arrays.
2. **K=2 merge correctness** -- already tested (5 tests passing per K-FUSION-ARCHITECTURE.md).
3. **Worker isolation** -- test that 2 concurrent workers calling `col_eval_relation_plan()` with the same session produce correct results (no races).
4. **Regression gate** -- all 15 workloads produce identical fact counts.

### Deferrable (Post Day 6)

1. Performance profiling (wall-time measurement).
2. K>=3 heap merge optimization.
3. Stress testing with varying worker counts.
4. Memory leak validation (valgrind).
5. DOOP-specific testing.

### Deferrable to Post-Launch

1. Hot-path optimization (merge comparator, memory allocation patterns).
2. Adaptive K-fusion (sequential for K=2 on small relations, parallel for K>=4).
3. Arena allocator (if malloc overhead is measured as significant).

---

## Contradictions and Concerns in Planning Documents

1. **BOTTLENECK-PROFILING-ANALYSIS.md claims "12 full sorts"** but the codebase already uses `col_op_consolidate_incremental_delta()` (line 2905), which does incremental merge-sort, not full qsort. The profiling data may be stale.

2. **Expected improvement oscillates wildly:** The documents variously claim 50-60% (BOTTLENECK doc), then correct to 30-40% (ROADMAP), then show a calculation arriving at 2.1% (IMPLEMENTATION-PATHS line 289), then correct again to 30-40%. The actual expected improvement for K=2 is unclear and should be measured before committing to a 3-week sprint.

3. **"Per-worker arena" referenced repeatedly** but `wl_arena_t` does not exist in the codebase. The workqueue.h file does not contain arena-related code. This is a design fiction that should be removed from planning docs to avoid confusion.

4. **"Evaluator rewrite" is a misnomer.** The actual change is adding a K-fusion dispatch branch to `col_eval_relation_plan()`. The semi-naive loop (`col_eval_stratum()`) is unchanged. The evaluator is not being rewritten -- it's being extended. This matters for risk perception: an extension is lower risk than a rewrite.

5. **The K-FUSION-ARCHITECTURE.md says "18/19 tests OK"** but the regression suite should be 15 workloads per the other documents. Unclear what the 19th test is and why it's EXPECTEDFAIL.

---

## Final Recommendation

### GO WITH CHANGES

The K-fusion strategy targets a real bottleneck and the core infrastructure is already in place. However, the following changes are required before engineering starts:

**Must Fix Before Starting:**

1. **Resolve the plan generation structural gap.** Define how `wl_plan_op_t` carries K-fusion sub-plan references. Propose adding a `void *opaque_data` field or a K-fusion-specific metadata struct. This is a 2-hour design task, not an implementation task, and it must be settled before Day 1.

2. **Drop the arena requirement.** Replace all "per-worker arena" references with "per-worker malloc isolation" (which is already thread-safe). Do not build an arena allocator for this feature.

3. **Address session thread-safety explicitly.** Document that the main thread must be blocked on `wait_all()` during worker execution, and that no session mutation is permitted during worker execution. Add a code comment and assertion.

4. **Revise CSPA improvement target to 15-25%** (not 30-40%). The empty-delta skip and incremental consolidation already capture some of the theoretical gains. Set DOOP <5 min as the primary success metric instead.

5. **Replace pairwise merge with heap merge for K>=3** before attempting DOOP validation. The code already exists in `col_op_consolidate_kway_merge()` -- extract and reuse.

**If these changes are made:** The 3-week timeline is achievable with a single engineer, and the risk profile is acceptable. The primary value is DOOP unblocking, with CSPA improvement as a secondary benefit.

**If these changes are not made:** The project will stall on Day 2-3 when the plan generation structural gap becomes apparent, and the thread-safety issue will surface as intermittent test failures around Day 5-6.

---

**Document Version:** 1.0
**Author:** Codex Implementation Specialist
**Review Status:** Independent analysis, not yet cross-reviewed
