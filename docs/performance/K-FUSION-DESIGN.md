# K-Fusion Design: Evaluator Rewrite Integration

**Date:** 2026-03-08
**Phase:** US-001 - Study & Design K-Fusion Integration
**Status:** Ready for Architecture Review

---

## Executive Summary

K-fusion replaces the sequential K-copy loop in `col_eval_relation_plan()` with parallel evaluation via the existing workqueue infrastructure. This document details the integration points, pseudo-code, and implementation strategy.

---

## Code Study Findings

### 1. Workqueue Infrastructure (wirelog/workqueue.h)

**Available API (5 functions):**
- `wl_workqueue_create(num_workers)` — Create thread pool
- `wl_workqueue_submit(wq, work_fn, ctx)` — Submit a task
- `wl_workqueue_wait_all(wq)` — Barrier (block until all tasks complete)
- `wl_workqueue_drain(wq)` — Synchronous fallback (single-threaded)
- `wl_workqueue_destroy(wq)` — Cleanup

**Threading Model:**
- Worker threads block on condition variable until tasks submitted
- Tasks dispatched from ring buffer (mutex-protected)
- Barrier semantics: `wait_all()` blocks until all submitted tasks complete
- Results available in caller-owned buffers (no shared state mutation)

**Per-Worker Arena Pattern (documented lines 44-62):**
```c
// Before submitting work:
for (uint32_t i = 0; i < num_workers; i++)
    worker_arenas[i] = wl_arena_create(capacity);

// Each work item's ctx embeds its own arena pointer:
struct work_ctx { wl_arena_t *arena; ... };

// After wait_all, arenas are freed by the main thread.
```

**Key Design:** Each worker gets a fresh, empty arena (NOT deep-copied). This is safe for bump allocators with no pre-existing state.

### 2. K-Way Merge Code (col_op_consolidate_kway_merge, lines 1498-1684)

**Algorithm:**
- Sort each segment in-place (qsort_r with row_cmp_fn)
- K=1: In-place dedup using memcmp
- K=2: 2-pointer merge with on-the-fly dedup
- K>2: Min-heap merge (not shown in first 80 lines, continues in code)

**Comparator:** `kway_row_cmp(row_i, row_j, nc)` — Returns:
- < 0 if row_i < row_j
- 0 if row_i == row_j
- > 0 if row_i > row_j

**Dedup Logic:** Tracks `last_row` and skips if duplicate (memcmp check)

**Key Finding:** All K-way merge logic is reusable for inline K-fusion merge.

### 3. Semi-Naive Evaluator Loop (col_eval_relation_plan, lines 2540-2730)

**Current Structure:**
```
for (iter = 0; iter < MAX_ITERATIONS; iter++) {
    // Register delta relations
    // Record per-relation snapshots

    for (uint32_t ri = 0; ri < nrels; ri++) {
        // Skip empty deltas

        eval_stack_t stack;
        eval_stack_init(&stack);

        int rc = col_eval_relation_plan(rp, &stack, sess);
        // ... process result
    }

    // Consolidate and check for fixed-point
    // ...
}
```

**Current K-Copy Pattern:** The plan expansion creates K separate copies of join+consolidate ops per relation. This forces sequential evaluation.

**Integration Point:** Between delta-relation registration and per-relation evaluation loop. The K-copy detection and dispatch should happen here.

---

## K-Fusion Integration Strategy

### Architecture

**New Node Type:** `COL_OP_K_FUSION`
- Metadata: `k` (number of copies), `ops[]` (array of K operation nodes), `output_id`
- Enum addition to `col_op_kind` in `columnar_nanoarrow.h`

**Struct Definition:**
```c
typedef struct {
    uint32_t k;              // Number of copies to evaluate
    col_plan_node_t **ops;   // Array of K operation pointers
    uint32_t output_id;      // Output relation ID
} col_op_k_fusion_t;
```

### Refactoring: col_eval_relation_plan()

**Current Flow (simplified):**
```c
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_plan_relation_t *rp = &sp->relations[ri];

    if (has_empty_forced_delta(rp, sess, iter)) continue;

    eval_stack_t stack;
    eval_stack_init(&stack);
    int rc = col_eval_relation_plan(rp, &stack, sess);
    // ... process stack result
}
```

**Proposed K-Fusion Flow:**
```c
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_plan_relation_t *rp = &sp->relations[ri];

    if (has_empty_forced_delta(rp, sess, iter)) continue;

    // NEW: Check if this is a K-fusion operation
    if (rp->plan->kind == COL_OP_K_FUSION) {
        col_op_k_fusion_t *fusion = (col_op_k_fusion_t *)rp->plan->data;

        // Parallel K-copy evaluation via workqueue
        int rc = col_eval_k_fusion(fusion, sess, arena);
        if (rc != 0) { /* error handling */ }

    } else {
        // Existing scalar evaluation path
        eval_stack_t stack;
        eval_stack_init(&stack);
        int rc = col_eval_relation_plan(rp, &stack, sess);
        // ... process stack result
    }
}
```

### K-Fusion Dispatch Implementation: `col_eval_k_fusion()`

**Pseudo-code:**
```c
int col_eval_k_fusion(col_op_k_fusion_t *fusion,
                      wl_session_t *sess,
                      wl_arena_t *main_arena)
{
    uint32_t k = fusion->k;

    // Step 1: Create per-worker arenas
    wl_arena_t **worker_arenas =
        (wl_arena_t **)malloc(k * sizeof(wl_arena_t *));
    for (uint32_t i = 0; i < k; i++) {
        worker_arenas[i] = wl_arena_create(ARENA_CAPACITY);
        if (!worker_arenas[i]) { /* error */ }
    }

    // Step 2: Create work queue
    wl_work_queue_t *wq = wl_workqueue_create(NUM_WORKERS);
    if (!wq) { /* error */ }

    // Step 3: Create work contexts (one per K copy)
    struct {
        col_plan_node_t *op;
        wl_arena_t *arena;
        col_rel_t *result;  // Output buffer for this copy
    } work_ctx[k];

    for (uint32_t i = 0; i < k; i++) {
        work_ctx[i].op = fusion->ops[i];
        work_ctx[i].arena = worker_arenas[i];
        work_ctx[i].result = NULL;  // Will be allocated by worker
    }

    // Step 4: Submit K tasks to workqueue
    for (uint32_t i = 0; i < k; i++) {
        int rc = wl_workqueue_submit(wq, col_eval_op_task, &work_ctx[i]);
        if (rc != 0) { /* error: queue capacity or invalid args */ }
    }

    // Step 5: Wait for all K tasks to complete (barrier)
    int rc = wl_workqueue_wait_all(wq);
    if (rc != 0) { /* error */ }

    // Step 6: Merge K sorted results in-place
    col_rel_t *results[k];
    for (uint32_t i = 0; i < k; i++) {
        results[i] = work_ctx[i].result;
    }
    col_rel_t *merged = col_rel_merge_k(results, k, main_arena);
    if (!merged) { /* error */ }

    // Step 7: Register merged result in session
    char rname[256];
    snprintf(rname, sizeof(rname), "<k-fusion-%u>", fusion->output_id);
    int rc = session_add_rel(sess, merged);
    if (rc != 0) { /* error */ }

    // Step 8: Cleanup
    wl_workqueue_destroy(wq);
    for (uint32_t i = 0; i < k; i++) {
        wl_arena_destroy(worker_arenas[i]);
    }
    free(worker_arenas);

    return 0;
}
```

### New Function: `col_rel_merge_k()`

**Purpose:** Merge K sorted relations with dedup

**Signature:**
```c
col_rel_t *col_rel_merge_k(col_rel_t **relations,
                           uint32_t k,
                           wl_arena_t *arena);
```

**Algorithm (reuses consolidate logic):**
1. Extract all relations into pointers
2. Min-heap merge (if K > 2) or 2-pointer merge (if K == 2)
3. On-the-fly dedup: track previous row, skip if duplicate
4. Output: merged relation with `k*max_nrows` capacity, actual deduplicated rows

**Thread-Safety:** Called sequentially by main thread AFTER `wait_all()` barrier, so no concurrency issues.

### Worker Task Function: `col_eval_op_task()`

**Receives work context with:**
- `op` — Operation node to evaluate
- `arena` — Per-worker arena (guaranteed unique)
- `result` — Output buffer reference (initialized by worker)

**Logic:**
```c
void col_eval_op_task(void *ctx) {
    struct work_ctx *wc = (struct work_ctx *)ctx;

    eval_stack_t stack;
    eval_stack_init(&stack);

    // Evaluate the K-copy operation with this worker's arena
    int rc = col_eval_relation_plan(wc->op, &stack, wc->arena);
    if (rc != 0) { /* handle error */ }

    // Extract result from stack into caller's buffer
    wc->result = eval_stack_pop(&stack);
    eval_stack_drain(&stack);
}
```

---

## Implementation Checklist

### Phase 1: Enum & Struct (US-002)
- [ ] Add `COL_OP_K_FUSION` to `col_op_kind` enum (columnar_nanoarrow.h)
- [ ] Define `col_op_k_fusion_t` struct
- [ ] Verify compilation (no warnings)
- [ ] Apply clang-format (llvm@18)

### Phase 2: Refactor col_eval_relation_plan() (US-003)
- [ ] Add K-fusion conditional in evaluation loop
- [ ] Dispatch to `col_eval_k_fusion()` when `op->kind == COL_OP_K_FUSION`
- [ ] Preserve backward compatibility for non-K-fusion ops
- [ ] Test compilation & clang-format

### Phase 3: Implement col_rel_merge_k() (US-004)
- [ ] Extract merge logic from `col_op_consolidate_kway_merge`
- [ ] Handle K=1 (passthrough), K=2 (2-pointer), K>2 (min-heap)
- [ ] Implement on-the-fly dedup
- [ ] Unit test: 2-way, 3-way, 4-way merges with duplicates

### Phase 4: Implement col_eval_k_fusion() (US-003 continuation)
- [ ] Create per-worker arenas
- [ ] Workqueue lifecycle (create, submit, wait_all, destroy)
- [ ] Work context setup
- [ ] Error handling & cleanup
- [ ] Merge results after barrier

### Phase 5: Testing (US-005-008)
- [ ] Unit tests: K=1, K=2, K=3 correctness
- [ ] Regression tests: All 15 workloads
- [ ] Performance profiling: wall-time & overhead
- [ ] DOOP validation: < 5 minutes

---

## Key Design Decisions

1. **Per-Worker Arenas, Not Cloned:**
   - Each worker gets a fresh arena (not deep-copied)
   - Safe because arena is a bump allocator
   - Reduces memory overhead vs full cloning

2. **Barrier Synchronization:**
   - `wait_all()` blocks until all K tasks complete
   - Ensures results are visible before merge
   - Sequential merge (no concurrency hazards)

3. **Collect-Then-Merge Pattern:**
   - Workers collect results into caller-owned buffers
   - Main thread merges sequentially
   - Avoids shared session state mutation

4. **Backward Compatibility:**
   - Non-K-fusion operations continue unchanged
   - Existing eval_stack flow preserved
   - Plan expansion still works for scalar ops

5. **Reuse Existing Code:**
   - `kway_row_cmp` comparator from consolidate
   - Merge algorithm proven in `col_op_consolidate_kway_merge`
   - Workqueue already tested in Phase B-lite

---

## Thread-Safety Analysis

**Per-Worker Arenas:**
- ✅ No shared mutable state
- ✅ Each worker has exclusive access to its arena
- ✅ Cleanup happens AFTER barrier (main thread only)

**Workqueue Synchronization:**
- ✅ `wait_all()` implements barrier semantics
- ✅ Mutex+CV ensures visibility of worker results
- ✅ Main thread merges sequentially after barrier

**Session State:**
- ✅ Workers do NOT mutate session state
- ✅ Results collected in local buffers
- ✅ Main thread updates session post-merge

---

## Files to Create/Modify

| File | Change | Scope |
|------|--------|-------|
| `columnar_nanoarrow.h` | Add `COL_OP_K_FUSION` enum, `col_op_k_fusion_t` struct | 20-30 lines |
| `columnar_nanoarrow.c` | Add `col_eval_k_fusion()`, `col_rel_merge_k()`, `col_eval_op_task()` | 200-250 lines |
| `columnar_nanoarrow.c` | Refactor `col_eval_relation_plan()` (lines 2540-2730) | 30-50 lines added |
| `tests/test_k_fusion.c` | NEW: Comprehensive K-fusion tests | 200-250 lines |
| `tests/meson.build` | Register new test | 2-3 lines |

**Total New Code:** ~500-600 lines (estimating conservatively)

---

## Next Steps

1. **Architecture Review:** Present this design to architect for approval
2. **US-002:** Implement enum and struct
3. **US-003:** Refactor col_eval_relation_plan() and implement col_eval_k_fusion()
4. **US-004:** Implement col_rel_merge_k()
5. **US-005-008:** Testing and validation

---

## References

- **Workqueue:** `wirelog/workqueue.h` (5-function API)
- **K-Way Merge:** `col_op_consolidate_kway_merge()` (lines 1498-1684)
- **Evaluator:** `col_eval_relation_plan()` (lines 2540-2730)
- **Semi-Naive Loop:** Lines 2540-2800 in `columnar_nanoarrow.c`

---

**Design Status:** ✅ COMPLETE - Ready for Architecture Review
