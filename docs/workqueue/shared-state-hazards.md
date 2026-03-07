# Shared-State Hazards in col_eval_stratum Parallelization

**Document**: Analysis of data race hazards in parallel relation evaluation
**Identified by**: Architect review
**Date**: 2026-03-06

---

## Overview

When parallelizing relation evaluation within `col_eval_stratum` (Phase B-lite), three distinct shared-state hazards exist in the result write-back path. All three must be addressed regardless of whether Option B-lite, Option D, or any other threading model is chosen. This document details each hazard and the structural mitigation: **collect-then-merge pattern**.

---

## Hazard 1: session_find_rel / session_add_rel Data Race

**Location**: `columnar_nanoarrow.c:1281-1293`

**Code**:
```c
col_rel_t *target = session_find_rel(sess, rp->name);  // line 1281
if (!target) {
    rc = session_add_rel(sess, result.rel);            // line 1293
}
```

**Problem**:
- `session_find_rel` (defined at `columnar_nanoarrow.c:264-269`) iterates `sess->rels[]` array looking for a matching relation name.
- `session_add_rel` (defined at `columnar_nanoarrow.c:273-302`) may grow the `sess->rels[]` array via `realloc`, invalidating pointers to the array returned by a concurrent `session_find_rel` call.
- **Race condition**: Thread A calls `session_find_rel`, gets a pointer to `sess->rels[i]`. Meanwhile, Thread B calls `session_add_rel`, which reallocates `sess->rels[]`. Thread A's subsequent dereference of the old pointer is a use-after-free.

**Severity**: Critical — use-after-free leads to memory corruption.

---

## Hazard 2: sess->nrels / sess->rel_cap Mutation

**Location**: `columnar_nanoarrow.c:247-248, 290-302`

**Code** (session structure):
```c
uint32_t nrels;      // line 247 - number of relations
uint32_t rel_cap;    // line 248 - capacity of rels[]
col_rel_t **rels;    // line 249 - relations array
```

**Code** (session_add_rel):
```c
if (sess->nrels >= sess->rel_cap) {
    sess->rel_cap *= 2;
    sess->rels = (col_rel_t **)realloc(sess->rels, sizeof(col_rel_t *) * sess->rel_cap);
    // ...
}
sess->rels[sess->nrels++] = rel;  // line 301
```

**Problem**:
- Multiple threads calling `session_add_rel` concurrently race on modifications to `sess->nrels`, `sess->rel_cap`, and the `sess->rels` pointer.
- Without synchronization (mutex), two threads may both observe `nrels >= rel_cap`, both call `realloc`, and both increment `nrels`, causing array bounds violations or lost writes.

**Severity**: Critical — unbounded growth, array bounds violation, lost updates.

---

## Hazard 3: Concurrent Append to Shared Target Relation

**Location**: `columnar_nanoarrow.c:1318`

**Code**:
```c
col_rel_t *target = session_find_rel(sess, rp->name);
// ...
rc = col_rel_append_all(target, result.rel);  // line 1318
```

**Problem**:
- If two `wl_plan_relation_t` entries within the same stratum produce results for the **same output relation** (same `rp->name`), they will both call `col_rel_append_all` on the same `target` object concurrently.
- `col_rel_append_all` modifies `target->nrows`, `target->data`, and potentially reallocates `target->data`.
- Concurrent writes to `target->nrows` and `target->data` (without synchronization) produce a data race.

**Example**: If the same dataflow rule is evaluated twice in one stratum (e.g., via semi-naive delta evaluation in Phase 2B+), both evaluations write their results to the same target relation.

**Severity**: Critical — data corruption, inconsistent relation state.

---

## Mitigation: Collect-Then-Merge Pattern

All three hazards are structurally prevented by the **collect-then-merge pattern**:

### Pattern Definition

```
Parallel phase:
  1. Each worker thread evaluates one relation plan (wl_plan_relation_t).
  2. Each worker produces an independent result (col_rel_t *result).
  3. Worker stores result in a thread-local or work-queue-managed buffer (NO writes to session).

Synchronization barrier (wait_all).

Sequential merge phase:
  4. Main thread collects all results from worker buffers.
  5. Main thread calls session_find_rel / session_add_rel to register targets.
  6. Main thread calls col_rel_append_all to merge each result into its target, sequentially.
```

### Why This Prevents the Hazards

| Hazard | How Prevented |
|--------|---------------|
| **Hazard 1** (find/add race) | Only main thread calls session_find_rel/session_add_rel (sequential). No concurrent array mutations. |
| **Hazard 2** (nrels/rel_cap mutation) | Only main thread modifies sess->nrels, sess->rel_cap, sess->rels. No concurrent updates. |
| **Hazard 3** (concurrent append to target) | Workers do not append to session relations. Main thread merges all results sequentially, one after another. |

### Implementation Sketch

**Phase B-lite executor must implement:**

```c
/* Worker evaluation */
struct WorkItem {
    wl_plan_relation_t *plan;
    col_rel_t *result;  // Filled in by worker
    int error;
};

void eval_one_relation(void *item_ptr, void *ctx) {
    struct WorkItem *item = (struct WorkItem *)item_ptr;
    // Evaluate plan into item->result
    // DO NOT call session_find_rel, session_add_rel, col_rel_append_all
    // Result is isolated, owned by this worker
}

/* Main thread orchestration in col_eval_stratum */
for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
    work_items[ri].plan = sp->relations[ri];
    wl_work_queue_submit(q, eval_one_relation, &work_items[ri], NULL);
}

wl_work_queue_wait_all(q);  // Barrier: all workers done

// Sequential merge phase
for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
    col_rel_t *result = work_items[ri].result;
    col_rel_t *target = session_find_rel(sess, result->relation_name);  // MAIN THREAD ONLY
    if (!target) {
        session_add_rel(sess, result);  // MAIN THREAD ONLY
    } else {
        col_rel_append_all(target, result);  // MAIN THREAD ONLY
    }
}
```

---

## Per-Worker Arena Concern

**Additional discovery**: The session allocator (`eval_arena`, allocated at `columnar_nanoarrow.c:1525` as a 256MB arena) must be cloned per worker thread to avoid allocator contention.

**Current code**:
```c
sess->eval_arena = wl_arena_create(256 * 1024 * 1024);
```

**Per-worker solution**: Before spawning workers, clone the arena for each worker:
```c
wl_arena_t *worker_arenas[num_workers];
for (int i = 0; i < num_workers; i++) {
    worker_arenas[i] = wl_arena_create(256 * 1024 * 1024);
}
```

Each worker gets its own arena via the work-queue context, avoiding lock contention in the allocator's internal synchronization.

---

## Sort Stability Decision (Related)

The `col_op_consolidate` operator (Phase A refactoring) must address `qsort` stability. This is a separate but related concern:

- Current `qsort` is not required to be stable.
- Recommendation: Declare **CONSOLIDATE output order as unspecified** (pragmatic, avoids stable-sort overhead).
- Alternative: Implement stable sort (custom merge sort, ~100 LOC).

**Decision required before Phase A baseline is established.**

---

## Testing Strategy

### Correctness Gate
1. Run all test suites with `-fsanitize=thread` (ThreadSanitizer enabled).
2. All three hazards would produce TSan reports if not properly mitigated.
3. **Pass criterion**: Zero TSan data race reports.

### Specific Test Cases
1. **Hazard 1 (find/add race)**: Evaluation scenario where multiple workers register new relations to the session.
2. **Hazard 2 (nrels/rel_cap mutation)**: Stratum with many independent relations (>10) forcing repeated reallocation.
3. **Hazard 3 (concurrent append)**: Stratum with two plans producing results for the same target relation (if supported by IR generator).

---

## References

- `columnar_nanoarrow.c:1261-1327` — col_eval_stratum loop (Phase B-lite parallelization target)
- `columnar_nanoarrow.c:264-269` — session_find_rel definition
- `columnar_nanoarrow.c:273-302` — session_add_rel definition
- `columnar_nanoarrow.c:247-249` — session structure (nrels, rel_cap, rels)
- `columnar_nanoarrow.c:1525` — eval_arena allocation
- `docs/workqueue/ADR-001-workqueue-introduction-strategy.md` — Collect-then-merge mandate
- `docs/workqueue/api-sketch.md` — Work-queue API and worker buffer management
