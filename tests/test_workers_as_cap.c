/*
 * test_workers_as_cap.c - workers=N is an upper bound, not eager allocation
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

#include <stdio.h>

static wl_plan_t *
build_plan(void)
{
    const char *src =
        ".decl edge(x: int32, y: int32)\n"
        ".decl reach(x: int32, y: int32)\n"
        "reach(x, y) :- edge(x, y).\n"
        "reach(x, z) :- reach(x, y), edge(y, z).\n";

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
    return rc == 0 ? plan : NULL;
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
    wl_plan_t *plan = build_plan();
    failed += expect("plan builds", plan != NULL);
    if (!plan)
        return 1;

    wl_session_t *base = NULL;
    int rc = wl_session_create(wl_backend_columnar(), plan, 8, &base);
    failed += expect("session accepts max worker cap", rc == 0 && base != NULL);
    if (rc != 0 || !base) {
        wl_plan_free(plan);
        return 1;
    }

    wl_col_session_t *sess = (wl_col_session_t *)base;
    failed += expect("workqueue is not eagerly allocated",
            sess->num_workers == 8 && sess->wq == NULL
            && sess->wq_workers == 0);
    failed += expect("TDD worker slots are not eagerly allocated",
            sess->tdd_workers == NULL && sess->tdd_workers_cap == 0
            && sess->tdd_workers_count == 0);

    rc = wl_columnar_session_ensure_workqueue(sess, 3);
    failed += expect("workqueue allocates selected active width",
            rc == 0 && sess->wq != NULL && sess->wq_workers == 3);
    rc = wl_columnar_session_ensure_tdd_worker_slots(sess, 3);
    failed += expect("TDD slots allocate selected active width",
            rc == 0 && sess->tdd_workers != NULL
            && sess->tdd_workers_cap == 3
            && sess->tdd_workers_count == 0);

    wl_session_destroy(base);
    wl_plan_free(plan);
    return failed == 0 ? 0 : 1;
}
