/*
 * test_worker_borrow_w2_tsan.c - Issue #592 R-5 + W=2 + TSan harness
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Pins the cross-coverage that the existing tests do NOT explicitly
 * combine in one place:
 *
 *   1. R-5 worker borrow path (col_worker_session_create)
 *      ←→ #579/#581 cover this single-threaded.
 *   2. W=2 workqueue concurrency
 *      ←→ #584 covers this with a STANDALONE arena (no R-5).
 *   3. TSan instrumentation under freeze guard
 *      ←→ #582 covers W=4×500/W=8×1000 with a STANDALONE arena.
 *
 * Race-surface-equivalence reasoning says the union is sufficient
 * (lookup/alloc on a borrowed pointer touches the same memory the
 * standalone-arena tests already TSan-stressed).  This file is the
 * explicit combined verification: two threads concurrently access
 * the SAME compound_arena pointer through the worker session's
 * borrowed slot while the coordinator holds it frozen.
 *
 * Test shape (200 iterations, comfortably below the 4095 epoch cap):
 *
 *   coordinator alloc()s a sentinel handle on coord->compound_arena,
 *   coordinator freeze()s,
 *   submit 2 workers via wl_workqueue_submit:
 *     each task reads worker_session.compound_arena (NOT coord's
 *     pointer), confirms equality with coord->compound_arena (R-5
 *     borrow), runs lookup() (must succeed) + alloc() (must refuse),
 *   wait_all,
 *   coordinator unfreeze + gc_epoch_boundary advances the epoch.
 *
 * The submit/wait_all release-acquire pair makes the non-atomic
 * arena->frozen flag observable to workers without explicit atomics
 * — the same synchronisation the #582 stress relies on.  If anyone
 * removes that synchronisation for the borrowed path, this test
 * surfaces the regression as a TSan data race.
 */

#include "../wirelog/arena/compound_arena.h"
#include "../wirelog/columnar/columnar_nanoarrow.h"
#include "../wirelog/columnar/internal.h"
#include "../wirelog/exec_plan_gen.h"
#include "../wirelog/passes/fusion.h"
#include "../wirelog/passes/jpp.h"
#include "../wirelog/passes/sip.h"
#include "../wirelog/session.h"
#include "../wirelog/wirelog.h"
#include "../wirelog/workqueue.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* Test harness                                                             */
/* ======================================================================== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                            \
        do {                                      \
            tests_run++;                          \
            printf("  [%d] %s", tests_run, name); \
        } while (0)
#define PASS()                 \
        do {                       \
            tests_passed++;        \
            printf(" ... PASS\n"); \
        } while (0)
#define FAIL(msg)                         \
        do {                                  \
            tests_failed++;                   \
            printf(" ... FAIL: %s\n", (msg)); \
        } while (0)

/* ======================================================================== */
/* Coordinator + worker fixture                                             */
/* ======================================================================== */

static wl_col_session_t *
make_coordinator(wl_plan_t **plan_out, wirelog_program_t **prog_out)
{
    wirelog_error_t err;
    wirelog_program_t *prog = wirelog_parse_string(
        ".decl edge(x: int32, y: int32)\n"
        ".decl path(x: int32, y: int32)\n"
        "path(x, y) :- edge(x, y).\n",
        &err);
    if (!prog)
        return NULL;
    wl_fusion_apply(prog, NULL);
    wl_jpp_apply(prog, NULL);
    wl_sip_apply(prog, NULL);

    wl_plan_t *plan = NULL;
    if (wl_plan_from_program(prog, &plan) != 0) {
        wirelog_program_free(prog);
        return NULL;
    }

    wl_session_t *session = NULL;
    if (wl_session_create(wl_backend_columnar(), plan, 2, &session) != 0) {
        wl_plan_free(plan);
        wirelog_program_free(prog);
        return NULL;
    }

    *plan_out = plan;
    *prog_out = prog;
    return COL_SESSION(session);
}

static void
cleanup_coordinator(wl_col_session_t *sess, wl_plan_t *plan,
    wirelog_program_t *prog)
{
    wl_session_destroy(&sess->base);
    wl_plan_free(plan);
    wirelog_program_free(prog);
}

/* ======================================================================== */
/* Worker task                                                              */
/* ======================================================================== */

typedef struct {
    /* Worker reads the arena through worker_session.compound_arena
     * (the R-5 borrowed pointer), NOT through coord->compound_arena
     * directly -- that's the integration this test is explicitly
     * verifying. */
    wl_col_session_t *worker_session;
    /* Snapshot of coord's pointer captured by the main thread before
     * submit; the worker compares against this to confirm R-5
     * pointer equality from the worker's POV. */
    wl_compound_arena_t *expected_arena_ptr;
    uint64_t sentinel_handle;
    uint32_t expected_payload_size;
    /* Outcome flags written by worker, read by coord after wait_all. */
    int pointer_matches;
    int frozen_observed;
    int lookup_ok;
    int alloc_blocked;
} borrow_task_t;

static void
worker_fn(void *ctx)
{
    borrow_task_t *t = (borrow_task_t *)ctx;
    wl_compound_arena_t *arena = t->worker_session->compound_arena;

    /* (1) R-5 borrow: worker session's compound_arena pointer is
     * the coord's pointer, byte-equal. */
    t->pointer_matches = (arena == t->expected_arena_ptr && arena != NULL);

    /* (2) Frozen flag observable through the borrowed pointer. */
    t->frozen_observed = (arena != NULL && arena->frozen);

    /* (3) Lookup of pre-freeze sentinel succeeds via borrowed ptr. */
    if (arena) {
        uint32_t out_size = 0;
        const void *payload = wl_compound_arena_lookup(arena,
                t->sentinel_handle, &out_size);
        t->lookup_ok = (payload != NULL
            && out_size == t->expected_payload_size);
    } else {
        t->lookup_ok = 0;
    }

    /* (4) Alloc through borrowed ptr while frozen must refuse. */
    if (arena) {
        uint64_t denied = wl_compound_arena_alloc(arena, 16u);
        t->alloc_blocked = (denied == WL_COMPOUND_HANDLE_NULL);
    } else {
        t->alloc_blocked = 0;
    }
}

/* ======================================================================== */
/* Test                                                                     */
/* ======================================================================== */

static void
test_w2_borrow_freeze_lookup(void)
{
    /* 200 iterations × W=2 workers; each iter advances current_epoch
     * by one (gc_epoch_boundary at the iter tail), so 200 stays
     * comfortably under the 4095 epoch cap. */
    static const uint32_t ITERATIONS = 200u;

    TEST("#592: R-5 borrow + W=2 + TSan, 200 freeze cycles");

    wl_plan_t *plan = NULL;
    wirelog_program_t *prog = NULL;
    wl_col_session_t *coord = make_coordinator(&plan, &prog);
    if (!coord) {
        FAIL("coordinator creation");
        return;
    }
    if (!coord->compound_arena) {
        cleanup_coordinator(coord, plan, prog);
        FAIL("coordinator compound_arena unexpectedly NULL");
        return;
    }

    /* Two worker sessions, R-5 borrow.  num_partitions=0 keeps the
     * fixture minimal: we are not testing partition transfer here,
     * just the borrowed-arena access pattern. */
    wl_col_session_t workers[2];
    memset(workers, 0, sizeof(workers));
    int rc = col_worker_session_create(coord, 0u, NULL, 0u, &workers[0]);
    if (rc != 0) {
        cleanup_coordinator(coord, plan, prog);
        FAIL("col_worker_session_create #0");
        return;
    }
    rc = col_worker_session_create(coord, 1u, NULL, 0u, &workers[1]);
    if (rc != 0) {
        col_worker_session_destroy(&workers[0]);
        cleanup_coordinator(coord, plan, prog);
        FAIL("col_worker_session_create #1");
        return;
    }

    /* Sanity: R-5 borrow holds at construction time. */
    if (workers[0].compound_arena != coord->compound_arena
        || workers[1].compound_arena != coord->compound_arena) {
        col_worker_session_destroy(&workers[0]);
        col_worker_session_destroy(&workers[1]);
        cleanup_coordinator(coord, plan, prog);
        FAIL("R-5 borrow violated at construction");
        return;
    }

    wl_work_queue_t *wq = wl_workqueue_create(2u);
    if (!wq) {
        col_worker_session_destroy(&workers[0]);
        col_worker_session_destroy(&workers[1]);
        cleanup_coordinator(coord, plan, prog);
        FAIL("workqueue create");
        return;
    }

    borrow_task_t tasks[2];
    int verdict = 0;
    for (uint32_t r = 0; r < ITERATIONS; r++) {
        /* Coord allocates a sentinel handle this iteration, sized
         * deterministically so the worker assertion is unambiguous. */
        uint64_t sentinel = wl_compound_arena_alloc(
            coord->compound_arena, 24u);
        if (sentinel == WL_COMPOUND_HANDLE_NULL) {
            printf(" ... FAIL: iter %u sentinel alloc returned NULL\n", r);
            verdict = 1;
            break;
        }

        wl_compound_arena_freeze(coord->compound_arena);

        for (uint32_t k = 0; k < 2u; k++) {
            tasks[k].worker_session = &workers[k];
            tasks[k].expected_arena_ptr = coord->compound_arena;
            tasks[k].sentinel_handle = sentinel;
            tasks[k].expected_payload_size = 24u;
            tasks[k].pointer_matches = 0;
            tasks[k].frozen_observed = 0;
            tasks[k].lookup_ok = 0;
            tasks[k].alloc_blocked = 0;
            if (wl_workqueue_submit(wq, worker_fn, &tasks[k]) != 0) {
                printf(" ... FAIL: submit iter %u worker %u\n", r, k);
                verdict = 1;
                break;
            }
        }
        if (verdict)
            break;

        if (wl_workqueue_wait_all(wq) != 0) {
            printf(" ... FAIL: wait_all iter %u\n", r);
            verdict = 1;
            break;
        }

        for (uint32_t k = 0; k < 2u; k++) {
            if (!tasks[k].pointer_matches) {
                printf(" ... FAIL: iter %u worker %u pointer mismatch\n",
                    r, k);
                verdict = 1;
                break;
            }
            if (!tasks[k].frozen_observed) {
                printf(" ... FAIL: iter %u worker %u missed frozen flag\n",
                    r, k);
                verdict = 1;
                break;
            }
            if (!tasks[k].lookup_ok) {
                printf(" ... FAIL: iter %u worker %u lookup failed\n",
                    r, k);
                verdict = 1;
                break;
            }
            if (!tasks[k].alloc_blocked) {
                printf(" ... FAIL: iter %u worker %u frozen alloc accepted\n",
                    r, k);
                verdict = 1;
                break;
            }
        }
        if (verdict)
            break;

        wl_compound_arena_unfreeze(coord->compound_arena);
        (void)wl_compound_arena_gc_epoch_boundary(coord->compound_arena);
    }

    wl_workqueue_destroy(wq);
    col_worker_session_destroy(&workers[0]);
    col_worker_session_destroy(&workers[1]);
    cleanup_coordinator(coord, plan, prog);

    if (verdict == 0)
        PASS();
}

/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int
main(void)
{
    printf("test_worker_borrow_w2_tsan (Issue #592)\n");
    printf("=======================================\n");

    test_w2_borrow_freeze_lookup();

    printf("\nResults: %d/%d passed, %d failed\n",
        tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
