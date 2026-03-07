/*
 * workqueue.c - wirelog Work Queue (Phase B-lite)
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * INTERNAL - not installed, not part of public API.
 *
 * Implements the 5-function interface declared in workqueue.h:
 *   wl_workqueue_create, wl_workqueue_submit, wl_workqueue_wait_all,
 *   wl_workqueue_drain, wl_workqueue_destroy.
 *
 * Thread pool design:
 *   - Fixed-size pool of pthreads, created at create() time.
 *   - Ring buffer of work items protected by a single mutex.
 *   - Workers block on a condition variable until work is available.
 *   - wait_all() blocks until all submitted items complete (barrier).
 *   - drain() executes all pending items synchronously on the caller thread.
 */

#include "workqueue.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Maximum pending tasks in the ring buffer. */
#define WL_WQ_RING_CAP 256u

/* ======================================================================== */
/* Work Item                                                                 */
/* ======================================================================== */

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
} wl_work_item_t;

/* ======================================================================== */
/* Work Queue (opaque)                                                       */
/* ======================================================================== */

struct wl_work_queue {
    /* Ring buffer of pending work items */
    wl_work_item_t ring[WL_WQ_RING_CAP];
    uint32_t head;  /* next item to dequeue (consumer) */
    uint32_t tail;  /* next slot to enqueue (producer)  */
    uint32_t count; /* items currently in ring          */

    /* Synchronisation */
    pthread_mutex_t mutex;
    pthread_cond_t work_avail; /* signalled when items are enqueued   */
    pthread_cond_t all_done;   /* signalled when in_flight reaches 0  */

    /* Barrier tracking */
    uint32_t submitted; /* total items submitted in current batch */
    uint32_t completed; /* total items completed in current batch */

    /* Shutdown flag */
    bool shutdown;

    /* Thread pool */
    pthread_t *threads;
    uint32_t num_workers;
};

/* ======================================================================== */
/* Worker Thread                                                             */
/* ======================================================================== */

static void *
worker_thread(void *arg)
{
    wl_work_queue_t *wq = (wl_work_queue_t *)arg;

    for (;;) {
        wl_work_item_t item;

        pthread_mutex_lock(&wq->mutex);

        /* Wait for work or shutdown */
        while (wq->count == 0 && !wq->shutdown)
            pthread_cond_wait(&wq->work_avail, &wq->mutex);

        if (wq->shutdown && wq->count == 0) {
            pthread_mutex_unlock(&wq->mutex);
            return NULL;
        }

        /* Dequeue one item */
        item = wq->ring[wq->head];
        wq->head = (wq->head + 1) % WL_WQ_RING_CAP;
        wq->count--;

        pthread_mutex_unlock(&wq->mutex);

        /* Execute outside the lock */
        item.fn(item.ctx);

        /* Signal completion */
        pthread_mutex_lock(&wq->mutex);
        wq->completed++;
        if (wq->completed == wq->submitted)
            pthread_cond_signal(&wq->all_done);
        pthread_mutex_unlock(&wq->mutex);
    }
}

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

wl_work_queue_t *
wl_workqueue_create(uint32_t num_workers)
{
    if (num_workers == 0)
        return NULL;

    wl_work_queue_t *wq = (wl_work_queue_t *)calloc(1, sizeof(wl_work_queue_t));
    if (!wq)
        return NULL;

    if (pthread_mutex_init(&wq->mutex, NULL) != 0) {
        free(wq);
        return NULL;
    }
    if (pthread_cond_init(&wq->work_avail, NULL) != 0) {
        pthread_mutex_destroy(&wq->mutex);
        free(wq);
        return NULL;
    }
    if (pthread_cond_init(&wq->all_done, NULL) != 0) {
        pthread_cond_destroy(&wq->work_avail);
        pthread_mutex_destroy(&wq->mutex);
        free(wq);
        return NULL;
    }

    wq->threads = (pthread_t *)calloc(num_workers, sizeof(pthread_t));
    if (!wq->threads) {
        pthread_cond_destroy(&wq->all_done);
        pthread_cond_destroy(&wq->work_avail);
        pthread_mutex_destroy(&wq->mutex);
        free(wq);
        return NULL;
    }

    wq->num_workers = num_workers;

    for (uint32_t i = 0; i < num_workers; i++) {
        if (pthread_create(&wq->threads[i], NULL, worker_thread, wq) != 0) {
            /* Partial creation: shut down already-created threads */
            wq->shutdown = true;
            pthread_cond_broadcast(&wq->work_avail);
            for (uint32_t j = 0; j < i; j++)
                pthread_join(wq->threads[j], NULL);
            free(wq->threads);
            pthread_cond_destroy(&wq->all_done);
            pthread_cond_destroy(&wq->work_avail);
            pthread_mutex_destroy(&wq->mutex);
            free(wq);
            return NULL;
        }
    }

    return wq;
}

int
wl_workqueue_submit(wl_work_queue_t *wq, void (*work_fn)(void *ctx), void *ctx)
{
    if (!wq || !work_fn)
        return -1;

    pthread_mutex_lock(&wq->mutex);

    if (wq->count >= WL_WQ_RING_CAP) {
        pthread_mutex_unlock(&wq->mutex);
        return -1; /* ring full */
    }

    wq->ring[wq->tail].fn = work_fn;
    wq->ring[wq->tail].ctx = ctx;
    wq->tail = (wq->tail + 1) % WL_WQ_RING_CAP;
    wq->count++;
    wq->submitted++;

    pthread_cond_signal(&wq->work_avail);
    pthread_mutex_unlock(&wq->mutex);

    return 0;
}

int
wl_workqueue_wait_all(wl_work_queue_t *wq)
{
    if (!wq)
        return -1;

    pthread_mutex_lock(&wq->mutex);

    while (wq->completed < wq->submitted)
        pthread_cond_wait(&wq->all_done, &wq->mutex);

    /* Reset counters for next batch */
    wq->submitted = 0;
    wq->completed = 0;

    pthread_mutex_unlock(&wq->mutex);

    return 0;
}

int
wl_workqueue_drain(wl_work_queue_t *wq)
{
    if (!wq)
        return -1;

    /*
     * Execute all pending items synchronously on the calling thread.
     * No lock needed for execution since drain bypasses the thread pool,
     * but we lock to dequeue safely.
     */
    for (;;) {
        wl_work_item_t item;

        pthread_mutex_lock(&wq->mutex);
        if (wq->count == 0) {
            /* Reset counters */
            wq->submitted = 0;
            wq->completed = 0;
            pthread_mutex_unlock(&wq->mutex);
            return 0;
        }

        item = wq->ring[wq->head];
        wq->head = (wq->head + 1) % WL_WQ_RING_CAP;
        wq->count--;
        pthread_mutex_unlock(&wq->mutex);

        item.fn(item.ctx);
    }
}

void
wl_workqueue_destroy(wl_work_queue_t *wq)
{
    if (!wq)
        return;

    /* Signal shutdown to all workers */
    pthread_mutex_lock(&wq->mutex);
    wq->shutdown = true;
    pthread_cond_broadcast(&wq->work_avail);
    pthread_mutex_unlock(&wq->mutex);

    /* Join all worker threads */
    for (uint32_t i = 0; i < wq->num_workers; i++)
        pthread_join(wq->threads[i], NULL);

    free(wq->threads);
    pthread_cond_destroy(&wq->all_done);
    pthread_cond_destroy(&wq->work_avail);
    pthread_mutex_destroy(&wq->mutex);
    free(wq);
}
