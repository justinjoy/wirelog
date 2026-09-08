/* Internal session options dispatch and validation tests. */

#include "wirelog/session.h"

#include <stdio.h>
#include <stdlib.h>

static int legacy_calls;
static int options_calls;
static void *observed_handle;

static int
fake_create(const wl_plan_t *plan, uint32_t num_workers, wl_session_t **out)
{
    (void)plan;
    (void)num_workers;
    legacy_calls++;
    *out = (wl_session_t *)calloc(1, sizeof(**out));
    return *out ? 0 : -1;
}

static int
fake_create_with_options(const wl_plan_t *plan, uint32_t num_workers,
    const wl_session_options_t *options, wl_session_t **out)
{
    (void)plan;
    (void)num_workers;
    options_calls++;
    observed_handle = options ? options->windows_job_handle : NULL;
    *out = (wl_session_t *)calloc(1, sizeof(**out));
    return *out ? 0 : -1;
}

static void
fake_destroy(wl_session_t *session)
{
    free(session);
}

int
main(void)
{
    static int borrowed_handle;
    const wl_compute_backend_t options_backend = {
        .name = "options",
        .session_create = fake_create,
        .session_destroy = fake_destroy,
        .session_create_with_options = fake_create_with_options,
    };
    const wl_compute_backend_t legacy_backend = {
        .name = "legacy",
        .session_create = fake_create,
        .session_destroy = fake_destroy,
    };
    wl_session_options_t options;
    wl_session_t *session = NULL;

    wl_session_options_init(&options);
    options.windows_job_handle = &borrowed_handle;
    if (wl_session_create_with_options(&options_backend, NULL, 1, &options,
        &session) != 0 || !session || options_calls != 1
        || observed_handle != &borrowed_handle) {
        fprintf(stderr, "options backend did not receive borrowed handle\n");
        return 1;
    }
    wl_session_destroy(session);

    session = NULL;
    if (wl_session_create(&options_backend, NULL, 1, &session) != 0
        || !session || legacy_calls != 1) {
        fprintf(stderr, "legacy creation path was not preserved\n");
        return 1;
    }
    wl_session_destroy(session);

    options.version++;
    session = NULL;
    if (wl_session_create_with_options(&options_backend, NULL, 1, &options,
        &session) == 0 || session || options_calls != 1) {
        fprintf(stderr, "invalid options version was accepted\n");
        return 1;
    }

    options.version = WL_SESSION_OPTIONS_VERSION;
    if (wl_session_create_with_options(&legacy_backend, NULL, 1, &options,
        &session) != 0 || !session || legacy_calls != 2) {
        fprintf(stderr, "legacy backend did not ignore options\n");
        return 1;
    }
    wl_session_destroy(session);
    return 0;
}
