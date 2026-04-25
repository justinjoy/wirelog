/*
 * test_session_compound_arena_lifecycle.c - Issue #559 lifecycle gateway test
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Verifies that wl_col_session_t owns a heap-allocated wl_compound_arena_t
 * across its lifecycle:
 *   - col_session_create allocates the arena (sess->compound_arena != NULL).
 *   - The arena is usable: a small wl_compound_arena_alloc returns a non-NULL
 *     handle that round-trips through the 20-bit session-seed field.
 *   - col_session_destroy frees the arena (no leaks under ASan).
 *
 * This is the Phase 0 gateway for the side-relation tier; downstream issues
 * replace the placeholder seed and wire the arena into operator paths.
 */

#include "../wirelog/backend.h"
#include "../wirelog/columnar/internal.h"
#include "../wirelog/exec_plan_gen.h"
#include "../wirelog/passes/fusion.h"
#include "../wirelog/passes/jpp.h"
#include "../wirelog/passes/sip.h"
#include "../wirelog/session.h"
#include "../wirelog/wirelog-parser.h"
#include "../wirelog/wirelog.h"

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
            return;                           \
        } while (0)

#define ASSERT(cond, msg)     \
        do {                      \
            if (!(cond)) {        \
                FAIL(msg);        \
            }                     \
        } while (0)

/* ======================================================================== */
/* Helper: build a minimal columnar plan                                    */
/* ======================================================================== */

static wl_plan_t *
build_plan(const char *src)
{
    wirelog_error_t err;
    wirelog_program_t *prog = wirelog_parse_string(src, &err);
    if (!prog)
        return NULL;

    wl_fusion_apply(prog, NULL);
    wl_jpp_apply(prog, NULL);
    wl_sip_apply(prog, NULL);

    wl_plan_t *plan = NULL;
    int rc = wl_plan_from_program(prog, &plan);
    wirelog_program_free(prog);
    if (rc != 0)
        return NULL;
    return plan;
}

/* ======================================================================== */
/* Test                                                                     */
/* ======================================================================== */

static void
test_session_compound_arena_lifecycle(void)
{
    TEST("session.compound_arena: create -> alloc -> destroy");

    wl_plan_t *plan = build_plan(".decl a(x: int32)\n"
            ".decl r(x: int32)\n"
            "r(x) :- a(x).\n");
    ASSERT(plan != NULL, "could not build plan");

    wl_session_t *session = NULL;
    int rc = wl_session_create(wl_backend_columnar(), plan, 1, &session);
    if (rc != 0 || !session) {
        wl_plan_free(plan);
        FAIL("wl_session_create failed");
    }

    wl_col_session_t *cs = COL_SESSION(session);
    ASSERT(cs->compound_arena != NULL,
        "compound_arena not allocated by col_session_create");

    /* Exercise one allocation so we know the arena is wired correctly. */
    uint64_t handle = wl_compound_arena_alloc(cs->compound_arena, 64u);
    if (handle == WL_COMPOUND_HANDLE_NULL) {
        wl_session_destroy(session);
        wl_plan_free(plan);
        FAIL("wl_compound_arena_alloc returned WL_COMPOUND_HANDLE_NULL");
    }

    /* Session seed propagation: low 20 bits of 0x53455353 ('SESS'). */
    if (wl_compound_handle_session(handle) != (0x53455353u & 0xFFFFFu)) {
        wl_session_destroy(session);
        wl_plan_free(plan);
        FAIL("handle session seed does not match placeholder constant");
    }

    wl_session_destroy(session);
    wl_plan_free(plan);
    PASS();
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

int
main(void)
{
    printf("test_session_compound_arena_lifecycle (Issue #559)\n");
    test_session_compound_arena_lifecycle();
    printf("\n%d run, %d passed, %d failed\n", tests_run, tests_passed,
        tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
