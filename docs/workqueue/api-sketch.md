# Work-Queue API Sketch: wl_work_queue_t

**Document**: Minimal work-queue interface design for Option B-lite
**Scope**: Internal API (not part of public session API)
**Implementation**: Phase B-lite (following Phase A global state elimination)
**Target LOC**: ~250-400 lines

---

## Purpose

The `wl_work_queue_t` abstraction provides a task-submission interface that:
1. Isolates parallelism concerns (thread pool implementation) from operator code
2. Enables future FPGA backend without modifying operator evaluation logic
3. Remains thin enough to debug alongside first concurrent code (mutexes + condvars only, no async callbacks)
4. Supports graceful degradation to single-threaded fallback (`drain` for embedded targets)

---

## Interface Definition

### Header: wirelog/backend/work_queue.h

```c
#ifndef WL_WORK_QUEUE_H
#define WL_WORK_QUEUE_H

#include <stdint.h>

typedef struct wl_work_queue wl_work_queue_t;

/* Work function signature: executed by a worker thread */
typedef void (*wl_work_fn)(void *item, void *ctx);

/* Create a work queue with specified worker count */
wl_work_queue_t *wl_work_queue_create(uint32_t num_workers);

/* Submit a work item to the queue
 *
 * @param q       Queue handle
 * @param fn      Worker function to execute
 * @param item    Opaque work item (caller-allocated, must be valid until wait_all returns)
 * @param ctx     Opaque context passed to fn
 *
 * @return 0 on success, ENOMEM if queue internal buffer is full
 */
int wl_work_queue_submit(wl_work_queue_t *q, wl_work_fn fn,
                          void *item, void *ctx);

/* Block until all submitted items have been processed
 *
 * This is a barrier: subsequent submit calls will enqueue new work,
 * and a new batch can be wait_all'd independently.
 */
void wl_work_queue_wait_all(wl_work_queue_t *q);

/* Drain queue synchronously (single-threaded fallback)
 *
 * Execute all submitted items sequentially on the calling thread.
 * Used on embedded targets where threading is not available or disabled.
 * Clears the queue after execution.
 */
void wl_work_queue_drain(wl_work_queue_t *q);

/* Destroy the queue and release all resources */
void wl_work_queue_destroy(wl_work_queue_t *q);

#endif /* WL_WORK_QUEUE_H */
```

---

## CPU Backend Implementation Notes

### Data Structures

```c
/* Internal: task ring buffer entry */
struct WorkTask {
    wl_work_fn fn;
    void *item;
    void *ctx;
};

/* Internal: thread pool state */
struct wl_work_queue {
    uint32_t num_workers;
    pthread_t *threads;

    /* Task queue */
    struct WorkTask *tasks;
    uint32_t task_capacity;
    uint32_t task_head;    /* Next task to execute */
    uint32_t task_tail;    /* Next task to enqueue */

    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t task_available;
    pthread_cond_t barrier;

    /* State */
    bool shutdown;         /* Set to true during destroy */
    uint32_t pending_tasks;  /* Tasks submitted but not yet executed */
    uint32_t completed_tasks; /* Tasks completed since last barrier */
};
```

### Algorithm: Producer-Consumer with Barrier

**Main thread** (producer):
1. `submit`: Acquire lock, enqueue task, signal `task_available`, release lock.
2. `wait_all`: Acquire lock, wait on `barrier` condition until `pending_tasks == 0`, release lock.

**Worker threads** (consumers):
1. Acquire lock.
2. While queue is not empty, dequeue task and execute.
3. After each task, decrement `pending_tasks`.
4. If `pending_tasks == 0`, signal `barrier` (wake main thread).
5. Release lock and loop to wait for next task_available.

### Pseudocode

```c
wl_work_queue_t *wl_work_queue_create(uint32_t num_workers) {
    q = calloc(1, sizeof(wl_work_queue_t));
    q->num_workers = num_workers;
    q->threads = malloc(num_workers * sizeof(pthread_t));
    q->tasks = malloc(1024 * sizeof(struct WorkTask));  /* Ring buffer */
    q->task_capacity = 1024;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->task_available, NULL);
    pthread_cond_init(&q->barrier, NULL);

    for (uint32_t i = 0; i < num_workers; i++) {
        pthread_create(&q->threads[i], NULL, worker_main, q);
    }
    return q;
}

int wl_work_queue_submit(wl_work_queue_t *q, wl_work_fn fn,
                          void *item, void *ctx) {
    pthread_mutex_lock(&q->lock);

    /* Enqueue task */
    q->tasks[q->task_tail].fn = fn;
    q->tasks[q->task_tail].item = item;
    q->tasks[q->task_tail].ctx = ctx;
    q->task_tail = (q->task_tail + 1) % q->task_capacity;
    q->pending_tasks++;

    pthread_cond_signal(&q->task_available);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

void wl_work_queue_wait_all(wl_work_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    while (q->pending_tasks > 0) {
        pthread_cond_wait(&q->barrier, &q->lock);
    }
    pthread_mutex_unlock(&q->lock);
}

void wl_work_queue_drain(wl_work_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    while (q->task_head != q->task_tail) {
        struct WorkTask task = q->tasks[q->task_head];
        q->task_head = (q->task_head + 1) % q->task_capacity;
        pthread_mutex_unlock(&q->lock);

        task.fn(task.item, task.ctx);  /* Execute synchronously */

        pthread_mutex_lock(&q->lock);
    }
    q->pending_tasks = 0;
    pthread_cond_broadcast(&q->barrier);
    pthread_mutex_unlock(&q->lock);
}

void *worker_main(void *arg) {
    wl_work_queue_t *q = (wl_work_queue_t *)arg;

    while (true) {
        pthread_mutex_lock(&q->lock);

        /* Wait for task or shutdown signal */
        while (q->task_head == q->task_tail && !q->shutdown) {
            pthread_cond_wait(&q->task_available, &q->lock);
        }

        if (q->shutdown && q->task_head == q->task_tail) {
            pthread_mutex_unlock(&q->lock);
            break;  /* Exit thread */
        }

        /* Dequeue and execute task */
        struct WorkTask task = q->tasks[q->task_head];
        q->task_head = (q->task_head + 1) % q->task_capacity;
        q->pending_tasks--;

        if (q->pending_tasks == 0) {
            pthread_cond_broadcast(&q->barrier);  /* Wake main thread */
        }

        pthread_mutex_unlock(&q->lock);

        task.fn(task.item, task.ctx);  /* Execute task */
    }
    return NULL;
}

void wl_work_queue_destroy(wl_work_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    pthread_cond_broadcast(&q->task_available);  /* Wake all workers */
    pthread_mutex_unlock(&q->lock);

    for (uint32_t i = 0; i < q->num_workers; i++) {
        pthread_join(q->threads[i], NULL);
    }

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->task_available);
    pthread_cond_destroy(&q->barrier);
    free(q->tasks);
    free(q->threads);
    free(q);
}
```

---

## Integration with col_eval_stratum

**Phase B-lite usage** (pseudocode):

```c
/* In col_eval_stratum, after relation evaluation (1261-1327) */

int col_eval_stratum(wl_col_session_t *sess, const wl_plan_stratum_t *sp) {
    if (sp->is_recursive) {
        /* Recursive strata: sequential evaluation only */
        return evaluate_recursive_stratum(sess, sp);
    }

    /* Non-recursive: parallelize via work-queue */
    wl_work_queue_t *q = wl_work_queue_create(sess->num_workers);

    struct WorkItem {
        wl_col_session_t *sess;
        const wl_plan_relation_t *plan;
        col_rel_t result;
        int error;
    };

    struct WorkItem *items = malloc(sp->relation_count * sizeof(struct WorkItem));

    /* Submit all relation evaluations */
    for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
        items[ri].sess = sess;
        items[ri].plan = sp->relations[ri];
        items[ri].error = 0;

        wl_work_queue_submit(q, worker_eval_relation, &items[ri], NULL);
    }

    /* Wait for all workers to complete */
    wl_work_queue_wait_all(q);

    /* Sequential merge phase: main thread merges results */
    for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
        if (items[ri].error) {
            /* Handle error */
            continue;
        }

        col_rel_t *target = session_find_rel(sess, items[ri].result.relation_name);
        if (!target) {
            session_add_rel(sess, &items[ri].result);
        } else {
            col_rel_append_all(target, &items[ri].result);
        }
    }

    wl_work_queue_destroy(q);
    free(items);
    return 0;
}

/* Worker function */
void worker_eval_relation(void *item_ptr, void *ctx) {
    struct WorkItem *item = (struct WorkItem *)item_ptr;

    /* Evaluate the plan into independent result buffer */
    eval_stack_t stack = eval_stack_create(item->sess);
    item->error = col_eval_plan(item->sess, item->plan, &item->result, &stack);
    eval_stack_destroy(&stack);

    /* Result is isolated; no writes to item->sess->rels[] */
}
```

---

## Error Handling

**Pattern**: Errors returned via work item context (not through the queue).

```c
struct WorkItem {
    // ... fields ...
    int error;  /* Set by worker, checked by main thread */
};

void worker_eval_relation(void *item_ptr, void *ctx) {
    struct WorkItem *item = (struct WorkItem *)item_ptr;
    item->error = 0;  /* Initialize */

    if (/* evaluation fails */) {
        item->error = SOME_ERROR_CODE;
        return;  /* Worker exits early; main thread checks item->error */
    }
}

/* Main thread post-barrier */
for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
    if (items[ri].error != 0) {
        // Handle error from worker ri
    }
}
```

---

## Embedded Fallback (drain)

On embedded targets where threading is disabled or unavailable:

```c
/* Compile-time or runtime flag */
if (!HAS_THREADING) {
    wl_work_queue_t *q = wl_work_queue_create(1);  /* Single worker */

    /* ... submit tasks as before ... */

    wl_work_queue_drain(q);  /* Execute synchronously instead of wait_all */

    /* Results available; merge as normal */
}
```

The `drain` interface is identical across all platforms; switching between threaded and single-threaded is transparent to the caller.

---

## Performance Considerations

### Lock Granularity
- Coarse grain: single lock protects entire queue (simple, but contention on submit/wait)
- Fine grain: separate locks for head/tail (complex, possible deadlock risk)
- **Recommendation**: Coarse grain is sufficient for Phase B-lite; ring buffer operations are O(1) and lock hold time is negligible

### Ring Buffer Sizing
- Start with 1024 entries (~16 KB for 64-bit pointers)
- If ring buffer overflows on submit, return ENOMEM (caller should reduce batch size or add retry loop)
- No dynamic resizing (simplifies implementation)

### FPGA Future-Proofing
- The `wl_work_fn` signature (void item, void ctx) is generic and works for FPGA: ctx can hold DMA buffer handle or kernel invocation state
- The `wait_all` barrier semantics map to DMA completion synchronization

---

## References

- `docs/workqueue/ADR-001-workqueue-introduction-strategy.md` — High-level decision and collect-then-merge mandate
- `docs/workqueue/shared-state-hazards.md` — Hazards prevented by task isolation
- `docs/threads/04-implementation-roadmap.md:263-277` — Phase B-full work-queue sketch (same interface, FPGA backend)
- `wirelog/backend/columnar_nanoarrow.c:1261-1327` — col_eval_stratum parallelization target
