/*
 * test_side_compound_delta.c - Delta callback regression for side compounds
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 */

#include "../wirelog/columnar/columnar_nanoarrow.h"
#include "../wirelog/exec_plan_gen.h"
#include "../wirelog/passes/fusion.h"
#include "../wirelog/passes/jpp.h"
#include "../wirelog/passes/sip.h"
#include "../wirelog/session.h"
#include "../wirelog/wirelog-parser.h"
#include "../wirelog/wirelog-types.h"
#include "../wirelog/wirelog.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DELTAS 64
#define MAX_COLS 8

typedef struct {
    int count;
    char relations[MAX_DELTAS][64];
    int64_t rows[MAX_DELTAS][MAX_COLS];
    uint32_t ncols[MAX_DELTAS];
    int32_t diffs[MAX_DELTAS];
} delta_collector_t;

static void
collect_delta(const char *relation, const int64_t *row, uint32_t ncols,
    int32_t diff, void *user_data)
{
    delta_collector_t *c = (delta_collector_t *)user_data;
    if (c->count >= MAX_DELTAS)
        return;
    int idx = c->count++;
    strncpy(c->relations[idx], relation, sizeof(c->relations[idx]) - 1);
    c->relations[idx][sizeof(c->relations[idx]) - 1] = '\0';
    c->ncols[idx] = ncols;
    c->diffs[idx] = diff;
    for (uint32_t i = 0; i < ncols && i < MAX_COLS; i++)
        c->rows[idx][i] = row[i];
}

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

static bool
has_delta(const delta_collector_t *deltas, const char *relation, int32_t diff)
{
    for (int i = 0; i < deltas->count; i++) {
        if (strcmp(deltas->relations[i], relation) == 0
            && deltas->diffs[i] == diff)
            return true;
    }
    return false;
}

static bool
has_row_delta(const delta_collector_t *deltas, const char *relation,
    const int64_t *row, uint32_t ncols, int32_t diff)
{
    for (int i = 0; i < deltas->count; i++) {
        if (strcmp(deltas->relations[i], relation) != 0
            || deltas->ncols[i] != ncols || deltas->diffs[i] != diff)
            continue;
        bool same = true;
        for (uint32_t c = 0; c < ncols; c++) {
            if (deltas->rows[i][c] != row[c]) {
                same = false;
                break;
            }
        }
        if (same)
            return true;
    }
    return false;
}

static int
run_case(uint32_t workers, bool pre_empty_step)
{
    const char *src =
        ".decl __compound_guard_1(handle: int64, arg0: int64)\n"
        ".decl __compound_guard_cmp_3(handle: int64, arg0: int64, arg1: int64, arg2: int64)\n"
        ".decl __compound_guard_and_2(handle: int64, arg0: int64, arg1: int64)\n"
        ".decl source(handle: int64, root: int64)\n"
        ".decl cmp(handle: int64, field: int64, op: int64, value: int64)\n"
        ".decl and_node(handle: int64, left: int64, right: int64)\n"
        "source(H, R) :- __compound_guard_1(H, R).\n"
        "cmp(H, F, O, V) :- __compound_guard_cmp_3(H, F, O, V).\n"
        "and_node(H, L, R) :- __compound_guard_and_2(H, L, R).\n";

    wl_plan_t *plan = build_plan(src);
    if (!plan) {
        fprintf(stderr, "could not build plan\n");
        return 1;
    }

    wl_session_t *session = NULL;
    int rc = wl_session_create(wl_backend_columnar(), plan, workers, &session);
    if (rc != 0 || !session) {
        fprintf(stderr, "could not create session: %d\n", rc);
        wl_plan_free(plan);
        return 1;
    }

    delta_collector_t deltas;
    memset(&deltas, 0, sizeof(deltas));
    wl_session_set_delta_cb(session, collect_delta, &deltas);

    if (pre_empty_step) {
        rc = wl_session_step(session);
        if (rc != 0 || deltas.count != 0) {
            fprintf(stderr,
                "empty pre-step failed: workers=%u rc=%d deltas=%d\n",
                workers, rc, deltas.count);
            wl_session_destroy(session);
            wl_plan_free(plan);
            return 1;
        }
    }

    uint64_t guard = 0;
    uint64_t cmp = 0;
    uint64_t and_node = 0;
    wirelog_compound_arg_t guard_args[] = {
        { WIRELOG_TYPE_INT64, 10 },
    };
    wirelog_compound_arg_t cmp_args[] = {
        { WIRELOG_TYPE_INT64, 20 },
        { WIRELOG_TYPE_INT64, 21 },
        { WIRELOG_TYPE_INT64, 22 },
    };
    wirelog_compound_arg_t and_args[] = {
        { WIRELOG_TYPE_INT64, 30 },
        { WIRELOG_TYPE_INT64, 31 },
    };

    rc = wl_session_make_compound(session, "guard", 1, guard_args, &guard);
    if (rc == 0)
        rc = wl_session_make_compound(session, "guard_cmp", 3, cmp_args, &cmp);
    if (rc == 0)
        rc = wl_session_make_compound(session, "guard_and", 2, and_args,
                &and_node);
    if (rc != 0 || guard == 0 || cmp == 0 || and_node == 0) {
        fprintf(stderr, "could not create compounds: workers=%u rc=%d\n",
            workers, rc);
        wl_session_destroy(session);
        wl_plan_free(plan);
        return 1;
    }

    rc = wl_session_step(session);
    int count_after_first_step = deltas.count;
    if (rc == 0)
        rc = wl_session_step(session);
    int count_after_second_step = deltas.count;
    if (rc == 0)
        rc = wl_session_step(session);

    wl_session_destroy(session);
    wl_plan_free(plan);

    if (rc != 0) {
        fprintf(stderr, "step failed: workers=%u rc=%d\n", workers, rc);
        return 1;
    }
    int64_t expected_source[] = { (int64_t)guard, 10 };
    int64_t expected_cmp[] = { (int64_t)cmp, 20, 21, 22 };
    int64_t expected_and[] = { (int64_t)and_node, 30, 31 };
    if (count_after_first_step != 3
        || !has_row_delta(&deltas, "source", expected_source, 2, +1)
        || !has_row_delta(&deltas, "cmp", expected_cmp, 4, +1)
        || !has_row_delta(&deltas, "and_node", expected_and, 3, +1)
        || has_delta(&deltas, "source", -1) || has_delta(&deltas, "cmp", -1)
        || has_delta(&deltas, "and_node", -1)) {
        fprintf(stderr,
            "expected exact initial projection deltas: workers=%u got=%d\n",
            workers, count_after_first_step);
        return 1;
    }
    if (count_after_second_step != count_after_first_step
        || deltas.count != count_after_first_step) {
        fprintf(stderr,
            "side compound projection emitted extra deltas: workers=%u first=%d second=%d third=%d\n",
            workers, count_after_first_step, count_after_second_step,
            deltas.count);
        return 1;
    }

    return 0;
}

int
main(void)
{
    if (run_case(1, false) != 0)
        return 1;
    if (run_case(4, false) != 0)
        return 1;
    if (run_case(1, true) != 0)
        return 1;
    return 0;
}
