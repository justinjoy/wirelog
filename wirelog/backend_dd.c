/*
 * backend_dd.c - Differential Dataflow compute backend
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <https://www.gnu.org/licenses/lgpl-3.0.html>.
 */

#include "backend.h"
#include "session.h"
#include "ffi/dd_ffi.h"

#include <stdlib.h>

/**
 * DD-specific session state embedding the base wl_session_t.
 *
 * Uses a persistent Rust-side session for incremental insert/step/delta,
 * and a batch worker for snapshot execution.
 */
typedef struct {
    wl_session_t base;

    /* Persistent session (Rust-side) for incremental operations */
    wl_dd_persistent_session_t *persistent;

    /* Batch worker and plan for snapshot execution */
    wl_dd_worker_t *worker;
    const wl_ffi_plan_t *plan;
} wl_dd_session_t;

static int
dd_session_create(const wl_ffi_plan_t *plan, uint32_t num_workers,
                  wl_session_t **out)
{
    wl_dd_session_t *s;
    wl_dd_persistent_session_t *persistent = NULL;
    wl_dd_worker_t *worker;
    int rc;

    if (!plan || !out)
        return -1;

    /* Create persistent session (Rust-side) */
    rc = wl_dd_session_create(plan, num_workers, &persistent);
    if (rc != 0)
        return rc;

    /* Create batch worker for snapshot */
    worker = wl_dd_worker_create(num_workers);

    s = (wl_dd_session_t *)calloc(1, sizeof(*s));
    if (!s) {
        wl_dd_session_destroy(persistent);
        if (worker)
            wl_dd_worker_destroy(worker);
        return -1;
    }

    s->persistent = persistent;
    s->worker = worker;
    s->plan = plan;

    *out = (wl_session_t *)s;
    return 0;
}

static void
dd_session_destroy(wl_session_t *session)
{
    wl_dd_session_t *s = (wl_dd_session_t *)session;
    if (s) {
        if (s->persistent)
            wl_dd_session_destroy(s->persistent);
        if (s->worker)
            wl_dd_worker_destroy(s->worker);
        free(s);
    }
}

static int
dd_session_insert(wl_session_t *session, const char *relation,
                  const int64_t *data, uint32_t num_rows, uint32_t num_cols)
{
    wl_dd_session_t *s = (wl_dd_session_t *)session;
    if (!s || !s->persistent)
        return -1;

    return wl_dd_session_insert(s->persistent, relation, data, num_rows,
                                num_cols);
}

static int
dd_session_remove(wl_session_t *session, const char *relation,
                  const int64_t *data, uint32_t num_rows, uint32_t num_cols)
{
    /* Insert-only MVP: remove is not supported */
    (void)session;
    (void)relation;
    (void)data;
    (void)num_rows;
    (void)num_cols;
    return -1;
}

static int
dd_session_step(wl_session_t *session)
{
    wl_dd_session_t *s = (wl_dd_session_t *)session;
    if (!s || !s->persistent)
        return -1;

    return wl_dd_session_step(s->persistent);
}

static void
dd_session_set_delta_cb(wl_session_t *session, wl_on_delta_fn callback,
                        void *user_data)
{
    wl_dd_session_t *s = (wl_dd_session_t *)session;
    if (!s || !s->persistent)
        return;

    wl_dd_session_set_delta_cb(s->persistent, (wl_dd_on_delta_fn)callback,
                               user_data);
}

static int
dd_session_snapshot(wl_session_t *session, wl_on_tuple_fn callback,
                    void *user_data)
{
    wl_dd_session_t *s = (wl_dd_session_t *)session;
    if (!s || !s->worker || !s->plan)
        return -1;

    return wl_dd_execute_cb(s->plan, s->worker, (wl_dd_on_tuple_fn)callback,
                            user_data);
}

static const wl_compute_backend_t dd_backend_vtable = {
    .name = "dd",
    .session_create = dd_session_create,
    .session_destroy = dd_session_destroy,
    .session_insert = dd_session_insert,
    .session_remove = dd_session_remove,
    .session_step = dd_session_step,
    .session_set_delta_cb = dd_session_set_delta_cb,
    .session_snapshot = dd_session_snapshot,
};

const wl_compute_backend_t *
wl_backend_dd(void)
{
    return &dd_backend_vtable;
}
