/*
 * test_constant_filter_pushdown.c - explicit equality pushdown regression
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 */

#include "../wirelog/columnar/internal.h"
#include "../wirelog/exec_plan_gen.h"
#include "../wirelog/passes/fusion.h"
#include "../wirelog/passes/jpp.h"
#include "../wirelog/passes/sip.h"
#include "../wirelog/session.h"
#include "../wirelog/wirelog.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct count_ctx {
    const char *relation;
    int64_t count;
} count_ctx_t;

static void
count_relation_cb(const char *relation, const int64_t *row, uint32_t ncols,
    void *user_data)
{
    count_ctx_t *ctx = (count_ctx_t *)user_data;
    (void)row;
    (void)ncols;
    if (ctx->relation && relation && strcmp(ctx->relation, relation) == 0)
        ctx->count++;
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

static int
run_case(const char *rule)
{
    char src[512];
    snprintf(src, sizeof(src),
        ".decl a(s: int32)\n"
        ".decl b(t: int32)\n"
        ".decl out(s: int32, t: int32)\n"
        "%s\n",
        rule);

    wl_plan_t *plan = build_plan(src);
    if (!plan)
        return 1;

    wl_session_t *session = NULL;
    int rc = wl_session_create(wl_backend_columnar(), plan, 1, &session);
    if (rc != 0 || !session) {
        wl_plan_free(plan);
        return 1;
    }

    wl_col_session_t *col_session = (wl_col_session_t *)session;
    col_session->join_output_limit = 3000;

    const uint32_t rows = 2000;
    int64_t *a_rows = (int64_t *)calloc(rows, sizeof(int64_t));
    int64_t *b_rows = (int64_t *)calloc(rows, sizeof(int64_t));
    if (!a_rows || !b_rows) {
        free(a_rows);
        free(b_rows);
        wl_session_destroy(session);
        wl_plan_free(plan);
        return 1;
    }

    for (uint32_t i = 0; i < rows; i++) {
        a_rows[i] = (int64_t)i;
        b_rows[i] = (int64_t)i;
    }
    b_rows[7] = 7;

    rc = wl_session_insert(session, "a", a_rows, rows, 1);
    if (rc == 0)
        rc = wl_session_insert(session, "b", b_rows, rows, 1);

    count_ctx_t ctx = { "out", 0 };
    if (rc == 0)
        rc = wl_session_snapshot(session, count_relation_cb, &ctx);

    free(a_rows);
    free(b_rows);
    wl_session_destroy(session);
    wl_plan_free(plan);

    return rc == 0 && ctx.count == rows ? 0 : 1;
}

static int
expect(const char *name, int ok)
{
    if (ok) {
        printf("%s ... PASS\n", name);
        return 0;
    }
    printf("%s ... FAIL\n", name);
    return 1;
}

int
main(void)
{
    int failed = 0;

    failed += expect("explicit var-constant equality pushed before join",
            run_case("out(s, t) :- a(s), b(t), t = 7.") == 0);
    failed += expect("explicit constant-var equality pushed before join",
            run_case("out(s, t) :- a(s), b(t), 7 = t.") == 0);

    return failed == 0 ? 0 : 1;
}
