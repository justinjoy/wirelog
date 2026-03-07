# Phase B-lite Workqueue Design

**Status**: Design complete, implementation pending
**Date**: 2026-03-07
**Depends on**: Phase A (global state elimination) — complete (commit 0578606)

---

## 1. Architecture: pthread-based CPU Backend

### Overview

The workqueue is a minimal task-submission abstraction (~300 LOC) that parallelises
non-recursive stratum evaluation in the columnar backend.  It uses a fixed-size
pthread thread pool with a shared ring buffer for task dispatch.

```
┌─────────────────────────────────────────────────────┐
│                    Main Thread                       │
│                                                     │
│  ┌─────────┐   ┌─────────┐         ┌─────────┐    │
│  │ submit  │──▶│  Ring    │◀──deq──│ Worker  │    │
│  │ submit  │──▶│  Buffer  │◀──deq──│ Worker  │    │
│  │ submit  │──▶│ (1024)  │◀──deq──│ Worker  │    │
│  └─────────┘   └─────────┘         └─────────┘    │
│       │              │                   │          │
│       │         mutex + condvar          │          │
│       ▼                                  ▼          │
│  wait_all() ◀──── barrier condvar ──── done         │
│       │                                             │
│       ▼                                             │
│  Sequential merge (session_find_rel / append_all)   │
└─────────────────────────────────────────────────────┘
```

### Components

| Component | Description |
|-----------|-------------|
| `wl_work_queue_t` | Opaque struct: ring buffer, mutex, condvars, thread handles |
| Ring buffer | Fixed 1024-entry circular buffer of `{work_fn, ctx}` pairs |
| Thread pool | `num_workers` pthreads created at init, joined at destroy |
| Barrier | `pending_count` tracked; `wait_all` blocks until zero |

### Lifecycle

```
create(N) → [submit, submit, ...] → wait_all() → [merge] → destroy()
                                        ↑                      │
                                        └── repeat batches ────┘
```

---

## 2. Thread Safety

### Synchronisation Primitives

All synchronisation uses POSIX mutexes and condition variables only (no atomics,
no lock-free structures).  This ensures full visibility in ThreadSanitizer and
GDB.

| Primitive | Purpose |
|-----------|---------|
| `pthread_mutex_t lock` | Protects ring buffer head/tail, pending count, shutdown flag |
| `pthread_cond_t task_available` | Workers wait here; signalled on submit |
| `pthread_cond_t barrier` | Main thread waits here; signalled when pending hits zero |

### Lock Protocol

**submit** (main thread):
1. Lock mutex
2. Enqueue `{work_fn, ctx}` at tail, advance tail
3. Increment `pending_count`
4. Signal `task_available`
5. Unlock mutex

**worker loop** (worker thread):
1. Lock mutex
2. While queue empty and not shutdown: wait on `task_available`
3. If shutdown and queue empty: unlock, exit thread
4. Dequeue task from head, advance head
5. Unlock mutex
6. **Execute task (outside lock)** — critical for parallelism
7. Lock mutex
8. Decrement `pending_count`; if zero, broadcast `barrier`
9. Unlock mutex
10. Goto 2

**wait_all** (main thread):
1. Lock mutex
2. While `pending_count > 0`: wait on `barrier`
3. Unlock mutex

### Memory Visibility

The mutex unlock in step 9 of the worker loop and the mutex acquire in step 1
of `wait_all` establish a happens-before relationship (POSIX guarantee).  This
ensures all writes by workers to their `ctx` buffers are visible to the main
thread after `wait_all` returns, without additional memory barriers.

### Per-Worker Arenas

The `wl_arena_t` bump allocator is **not thread-safe** (no internal locking).
Each worker must receive its own arena via the `ctx` parameter:

```c
struct eval_work_ctx {
    const wl_plan_relation_t *plan;
    wl_arena_t               *arena;    /* per-worker, fresh */
    col_rel_t                 result;   /* output buffer */
    int                       error;    /* 0 = success */
};

/* Before submitting: */
for (uint32_t i = 0; i < count; i++) {
    items[i].arena = wl_arena_create(arena_capacity);
}

/* After wait_all: */
for (uint32_t i = 0; i < count; i++) {
    wl_arena_free(items[i].arena);
}
```

**Why fresh arenas, not clones**: `wl_arena_t` is a bump allocator.  "Cloning"
would mean copying the used portion, but workers need empty scratch space, not
copies of prior allocations.  Creating a fresh arena of the same capacity is
the correct approach.

---

## 3. Integration: Non-Recursive Strata

### Parallelisation Target

Only **non-recursive strata** (`is_recursive == false`) are parallelised.
Recursive strata require iterative fixed-point convergence with delta tracking
between iterations — this inherently sequential dependency prevents
parallelisation at the relation level.

### Integration Point

In `col_eval_stratum()` (`columnar_nanoarrow.c`):

```c
int col_eval_stratum(wl_col_session_t *sess, const wl_plan_stratum_t *sp) {
    if (sp->is_recursive) {
        return eval_recursive_stratum(sess, sp);  /* sequential */
    }

    /* Non-recursive: parallel evaluation via workqueue */
    wl_work_queue_t *wq = wl_workqueue_create(sess->num_workers);
    if (!wq) return -1;

    uint32_t n = sp->relation_count;
    struct eval_work_ctx *items = calloc(n, sizeof(*items));
    if (!items) { wl_workqueue_destroy(wq); return -1; }

    /* Phase 1: Submit — each relation evaluated independently */
    for (uint32_t i = 0; i < n; i++) {
        items[i].plan  = &sp->relations[i];
        items[i].arena = wl_arena_create(ARENA_CAPACITY);
        items[i].error = 0;
        wl_workqueue_submit(wq, eval_relation_worker, &items[i]);
    }

    /* Phase 2: Barrier — wait for all workers */
    wl_workqueue_wait_all(wq);

    /* Phase 3: Merge — main thread only, sequential */
    int rc = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (items[i].error) { rc = -1; continue; }
        col_rel_t *target = session_find_rel(sess, items[i].result.name);
        if (!target)
            session_add_rel(sess, &items[i].result);
        else
            col_rel_append_all(target, &items[i].result);
    }

    /* Cleanup */
    for (uint32_t i = 0; i < n; i++)
        wl_arena_free(items[i].arena);
    free(items);
    wl_workqueue_destroy(wq);
    return rc;
}
```

### Worker Function

```c
static void eval_relation_worker(void *ctx) {
    struct eval_work_ctx *item = (struct eval_work_ctx *)ctx;
    /* Evaluate plan into item->result using item->arena.
     * NO writes to session state (sess->rels, sess->nrels, etc.).
     * All output goes to item->result (caller-owned buffer). */
    item->error = col_eval_plan(item->plan, item->arena, &item->result);
}
```

---

## 4. Collect-Then-Merge Pattern

This pattern is **mandatory** (see ADR-001) and structurally prevents all three
shared-state hazards identified in `shared-state-hazards.md`:

| Phase | Thread | Accesses | Hazards Prevented |
|-------|--------|----------|-------------------|
| Submit + Execute | Worker threads | Read-only: plan, EDB relations. Write: per-worker result buffer, per-worker arena | Workers never touch session state |
| Barrier | — | Synchronisation point | Ensures visibility |
| Merge | Main thread only | Read/write: sess->rels[], sess->nrels | Sequential access eliminates all races |

### What Workers May Access

| Resource | Access | Safe? |
|----------|--------|-------|
| `wl_plan_relation_t` (plan) | Read-only | Yes — immutable after plan generation |
| EDB relations (`col_rel_t`) | Read-only | Yes — populated before evaluation, not modified during |
| `eval_work_ctx.arena` | Write (own) | Yes — per-worker, no sharing |
| `eval_work_ctx.result` | Write (own) | Yes — per-worker, no sharing |
| `sess->rels[]` | **FORBIDDEN** | No — main thread only during merge |
| `sess->nrels` / `sess->rel_cap` | **FORBIDDEN** | No — main thread only during merge |

---

## 5. Code Skeleton: wl_workqueue_submit Usage (~50 lines)

```c
#include "workqueue.h"
#include "arena/arena.h"

#define ARENA_CAP (64 * 1024 * 1024)  /* 64 MB per worker */

struct work_item {
    const wl_plan_relation_t *plan;
    wl_arena_t               *arena;
    col_rel_t                 result;
    int                       error;
};

static void do_work(void *ctx) {
    struct work_item *w = (struct work_item *)ctx;
    w->error = col_eval_plan(w->plan, w->arena, &w->result);
}

int parallel_eval_stratum(wl_col_session_t *sess,
                          const wl_plan_stratum_t *sp) {
    uint32_t n = sp->relation_count;
    wl_work_queue_t *wq = wl_workqueue_create(sess->num_workers);
    struct work_item *items = calloc(n, sizeof(*items));

    /* Submit phase */
    for (uint32_t i = 0; i < n; i++) {
        items[i].plan  = &sp->relations[i];
        items[i].arena = wl_arena_create(ARENA_CAP);
        wl_workqueue_submit(wq, do_work, &items[i]);
    }

    /* Barrier */
    wl_workqueue_wait_all(wq);

    /* Sequential merge */
    int rc = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (items[i].error) { rc = -1; continue; }
        col_rel_t *tgt = session_find_rel(sess, items[i].result.name);
        if (!tgt)
            session_add_rel(sess, &items[i].result);
        else
            col_rel_append_all(tgt, &items[i].result);
        wl_arena_free(items[i].arena);
    }

    free(items);
    wl_workqueue_destroy(wq);
    return rc;
}
```

---

## 6. Design Review: Data Race Hazards

### Identified Hazards

All hazards from `shared-state-hazards.md` are addressed:

| # | Hazard | Location | Mitigation |
|---|--------|----------|------------|
| 1 | `session_find_rel`/`session_add_rel` race | `columnar_nanoarrow.c:1281-1293` | Merge phase is main-thread-only |
| 2 | `sess->nrels`/`sess->rel_cap` mutation | `columnar_nanoarrow.c:247-248` | Merge phase is main-thread-only |
| 3 | Concurrent `col_rel_append_all` | `columnar_nanoarrow.c:1318` | Merge phase is main-thread-only |

### Additional Locking Points

| Point | Lock | Duration |
|-------|------|----------|
| Ring buffer enqueue (submit) | `lock` | O(1) — pointer copy + index advance |
| Ring buffer dequeue (worker) | `lock` | O(1) — pointer copy + index advance |
| Task execution | **unlocked** | Varies — entire relation evaluation |
| Pending count update | `lock` | O(1) — decrement + conditional signal |

### Ring Buffer Overflow

If `submit` is called when the ring buffer (1024 entries) is full, it returns
`-1`.  This cannot happen in practice because stratum relation counts are
bounded by program size (typically < 100).

### Shutdown Race

`wl_workqueue_destroy` sets `shutdown = true` under the lock and broadcasts
`task_available`.  Workers check `shutdown` after waking and exit if the queue
is empty.  The main thread then joins all workers — no race.

---

## 7. Performance Model

### Amdahl's Law Analysis

For non-recursive stratum evaluation, the parallelisable fraction depends on
the ratio of relation evaluation time to merge time.

Let:
- `P` = fraction of time in parallel evaluation (worker functions)
- `S` = fraction of time in sequential merge + queue overhead
- `P + S = 1`

| Workers | Speedup (P=0.8) | Speedup (P=0.9) | Speedup (P=0.95) |
|---------|-----------------|-----------------|-------------------|
| 1       | 1.00x           | 1.00x           | 1.00x             |
| 2       | 1.67x           | 1.82x           | 1.90x             |
| 4       | 2.50x           | 3.08x           | 3.48x             |
| 8       | 3.33x           | 4.71x           | 5.93x             |

### Expected P Values by Workload

| Workload | Relations/Stratum | Expected P | Rationale |
|----------|-------------------|------------|-----------|
| TC (4 nodes) | 1-2 | ~0.0 | Too few relations; overhead dominates |
| Reach (graph) | 2-4 | ~0.7 | Moderate parallelism |
| Polonius | 10-50+ | ~0.9 | Many independent relations per stratum |

### Overhead Sources (S)

| Source | Estimated Cost |
|--------|---------------|
| Mutex acquire/release per task | ~100 ns |
| Condition variable signal | ~200 ns |
| Arena create per worker | ~1 us (mmap/malloc) |
| Sequential merge (append_all) | O(total_rows) — memory copy |
| Thread pool create/destroy | ~50 us (pthread_create × N) |

### Optimisation: Queue Reuse

For production use, the `wl_work_queue_t` should be created once per session
(not per stratum) to amortise thread creation cost.  The integration sketch
above creates per-stratum for clarity; the actual implementation should cache
the queue in `wl_col_session_t`.

### Scaling Recommendations

| Workers | Target Platform |
|---------|-----------------|
| 1 | Embedded / debugging / baseline |
| 2 | Laptop (dual-core, conservative) |
| 4 | Desktop / CI (quad-core) |
| 8 | Server (8+ cores, Polonius-scale workloads) |

Beyond 8 workers, diminishing returns are expected due to merge phase
serialisation and memory bandwidth saturation.

---

## 8. Related Documents

- `docs/workqueue/ADR-001-workqueue-introduction-strategy.md` — Decision record
- `docs/workqueue/api-sketch.md` — Earlier API sketch (superceded by `wirelog/workqueue.h`)
- `docs/workqueue/shared-state-hazards.md` — Data race hazard analysis
- `wirelog/workqueue.h` — Canonical header (5-function interface)
- `wirelog/arena/arena.h` — Arena allocator (per-worker cloning target)
