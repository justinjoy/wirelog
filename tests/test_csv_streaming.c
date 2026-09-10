/*
 * test_csv_streaming.c - bounded built-in CSV reader tests
 */

#include "../wirelog/io/csv_reader.h"
#include "../wirelog/intern.h"
#include "../wirelog/columnar/memory_governor.h"
#include "../wirelog/wirelog-types.h"
#include "test_tmpdir.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t callbacks;
    uint32_t rows;
    uint32_t fail_at;
    int64_t values[16][2];
} stream_observer_t;

static int64_t
intern_cb(void *opaque, const char *value)
{
    return wl_intern_put((wl_intern_t *)opaque, value);
}

static int
observe_rows(void *opaque, const int64_t *rows, uint32_t nrows,
    uint32_t ncols)
{
    stream_observer_t *obs = (stream_observer_t *)opaque;
    if (!obs || !rows || ncols != 2)
        return EINVAL;
    obs->callbacks++;
    if (obs->fail_at != 0 && obs->callbacks == obs->fail_at)
        return ENOMEM;
    for (uint32_t i = 0; i < nrows; i++) {
        if (obs->rows >= 16)
            return EOVERFLOW;
        memcpy(obs->values[obs->rows++], rows + (size_t)i * ncols,
            sizeof(obs->values[0]));
    }
    return 0;
}

static int
write_fixture(char *path, size_t path_size, const char *name,
    const char *contents)
{
    test_tmppath(path, path_size, name);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(contents, f);
    fclose(f);
    return 0;
}

static int
test_batches_and_strings(void)
{
    char path[512];
    if (write_fixture(path, sizeof(path), "wirelog_csv_streaming.csv",
        "1,\"alpha\"\n2,\"beta\"\n2,\"alpha\"\n3,\"gamma\"\n"
        "5,\"delta\"\n") != 0)
        return 1;

    wirelog_column_type_t types[] = {
        WIRELOG_TYPE_INT64, WIRELOG_TYPE_STRING,
    };
    wl_intern_t *intern = wl_intern_create();
    stream_observer_t obs = {0};
    int rc = wl_csv_read_file_via_ctx_stream(path, ',', types, 2, 2,
            observe_rows, &obs, intern_cb, intern);
    remove(path);
    wl_intern_free(intern);
    return rc == 0 && obs.callbacks == 3 && obs.rows == 5
           && obs.values[0][0] == 1 && obs.values[1][0] == 2
           && obs.values[2][0] == 2 && obs.values[4][0] == 5 ? 0 : 1;
}

static int
test_callback_failure(void)
{
    char path[512];
    if (write_fixture(path, sizeof(path), "wirelog_csv_streaming_fail.csv",
        "1,one\n2,two\n3,three\n4,four\n") != 0)
        return 1;
    wirelog_column_type_t types[] = {
        WIRELOG_TYPE_INT64, WIRELOG_TYPE_STRING,
    };
    wl_intern_t *intern = wl_intern_create();
    stream_observer_t obs = {.fail_at = 2};
    int rc = wl_csv_read_file_via_ctx_stream(path, ',', types, 2, 2,
            observe_rows, &obs, intern_cb, intern);
    remove(path);
    wl_intern_free(intern);
    return rc != 0 && obs.callbacks == 2 && obs.rows == 2 ? 0 : 1;
}

static int
test_admission_denial(void)
{
    char path[512];
    if (write_fixture(path, sizeof(path), "wirelog_csv_streaming_budget.csv",
        "1,one\n") != 0)
        return 1;

    wirelog_column_type_t types[] = {
        WIRELOG_TYPE_INT64, WIRELOG_TYPE_STRING,
    };
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_resolution_t resolution = {
        .budget_bytes = 1,
        .headroom_bytes = 0,
        .usable_bytes = 1,
        .mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING,
        .source = WL_COLUMNAR_MEMORY_SOURCE_ENV,
        .status = WL_COLUMNAR_MEMORY_OK,
    };
    wl_columnar_memory_governor_ref_t *ref
        = wl_columnar_memory_governor_ref_create(&resolution);
    stream_observer_t obs = {0};
    int rc = ref
        ? wl_csv_read_file_via_ctx_stream_admitted(path, ',', types, 2, 2,
            observe_rows, &obs, intern_cb, intern,
            wl_columnar_memory_governor_ref_get(ref))
        : WL_CSV_ERR_MEMORY;
    int clean = ref != NULL
        && wl_columnar_memory_reserved(
        wl_columnar_memory_governor_ref_get(ref)) == 0
        && rc == WL_CSV_ERR_MEMORY && obs.rows == 0;
    if (ref)
        wl_columnar_memory_governor_ref_release(ref);
    wl_intern_free(intern);
    remove(path);
    return clean ? 0 : 1;
}

static int
test_admission_release(void)
{
    char path[512];
    if (write_fixture(path, sizeof(path), "wirelog_csv_streaming_release.csv",
        "1,one\n2,two\n") != 0)
        return 1;

    wirelog_column_type_t types[] = {
        WIRELOG_TYPE_INT64, WIRELOG_TYPE_STRING,
    };
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_resolution_t resolution = {
        .budget_bytes = UINT64_C(64) * 1024 * 1024,
        .headroom_bytes = 0,
        .usable_bytes = UINT64_C(64) * 1024 * 1024,
        .mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING,
        .source = WL_COLUMNAR_MEMORY_SOURCE_ENV,
        .status = WL_COLUMNAR_MEMORY_OK,
    };
    wl_columnar_memory_governor_ref_t *ref
        = wl_columnar_memory_governor_ref_create(&resolution);
    stream_observer_t obs = {0};
    int rc = ref
        ? wl_csv_read_file_via_ctx_stream_admitted(path, ',', types, 2, 2,
            observe_rows, &obs, intern_cb, intern,
            wl_columnar_memory_governor_ref_get(ref))
        : WL_CSV_ERR_MEMORY;
    int clean = ref != NULL
        && rc == 0 && obs.rows == 2
        && wl_columnar_memory_reserved(
        wl_columnar_memory_governor_ref_get(ref)) == 0;
    if (ref)
        wl_columnar_memory_governor_ref_release(ref);
    wl_intern_free(intern);
    remove(path);
    return clean ? 0 : 1;
}

/* Issue #1431: a program-owned intern table attached to an exhausted
 * governor denies the first unique string a CSV load tries to intern.  The
 * load must report an error, the ids interned before the load must still
 * resolve, and neither governor may keep a stale reservation. */
static int
test_intern_denial_keeps_prior_ids(void)
{
    char path[512];
    if (write_fixture(path, sizeof(path), "wirelog_csv_streaming_intern.csv",
        "1,alpha\n2,beta\n") != 0)
        return 1;

    wirelog_column_type_t types[] = {
        WIRELOG_TYPE_INT64, WIRELOG_TYPE_STRING,
    };
    wl_columnar_memory_resolution_t generous = {
        .budget_bytes = 1u << 20,
            .headroom_bytes = 0,
            .usable_bytes = 1u << 20,
            .mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING,
            .source = WL_COLUMNAR_MEMORY_SOURCE_ENV,
            .status = WL_COLUMNAR_MEMORY_OK,
    };
    wl_columnar_memory_resolution_t exact = generous;
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *probe
        = wl_columnar_memory_governor_ref_create(&generous);
    wl_columnar_memory_governor_ref_t *csv_ref
        = wl_columnar_memory_governor_ref_create(&generous);
    wl_columnar_memory_governor_ref_t *tight = NULL;
    stream_observer_t obs = {0};
    uint64_t footprint = 0;
    int rc = -1;
    int clean = 0;

    if (!intern || !probe || !csv_ref)
        goto out;
    /* "alpha" is interned before the load; measure the table's footprint
     * through a probe governor, then re-attach it to a governor whose budget
     * is exactly that footprint so any growth is denied. */
    if (wl_intern_put(intern, "alpha") != 0
        || wl_intern_attach_memory_governor(intern, probe) != 0)
        goto out;
    footprint = wl_columnar_memory_reserved(
        wl_columnar_memory_governor_ref_get(probe));
    if (footprint == 0 || wl_intern_detach_memory_governor(intern, probe) != 0)
        goto out;
    exact.budget_bytes = footprint;
    exact.usable_bytes = footprint;
    tight = wl_columnar_memory_governor_ref_create(&exact);
    if (!tight || wl_intern_attach_memory_governor(intern, tight) != 0)
        goto out;

    rc = wl_csv_read_file_via_ctx_stream_admitted(path, ',', types, 2, 2,
            observe_rows, &obs, intern_cb, intern,
            wl_columnar_memory_governor_ref_get(csv_ref));
    clean = rc != 0
        && wl_intern_count(intern) == 1u
        && wl_intern_get(intern, "alpha") == 0
        && strcmp(wl_intern_reverse(intern, 0), "alpha") == 0
        && wl_intern_get(intern, "beta") == -1
        && wl_columnar_memory_reserved(
        wl_columnar_memory_governor_ref_get(tight)) == footprint
        && wl_columnar_memory_reserved(
        wl_columnar_memory_governor_ref_get(csv_ref)) == 0;
    if (!clean)
        fprintf(stderr, "intern denial: rc=%d count=%u reserved=%llu/%llu\n",
            rc, (unsigned)wl_intern_count(intern),
            (unsigned long long)wl_columnar_memory_reserved(
                wl_columnar_memory_governor_ref_get(tight)),
            (unsigned long long)footprint);
out:
    if (intern)
        wl_intern_free(intern);
    if (tight)
        clean = clean && wl_columnar_memory_reserved(
            wl_columnar_memory_governor_ref_get(tight)) == 0;
    if (tight)
        wl_columnar_memory_governor_ref_release(tight);
    if (csv_ref)
        wl_columnar_memory_governor_ref_release(csv_ref);
    if (probe)
        wl_columnar_memory_governor_ref_release(probe);
    remove(path);
    return clean ? 0 : 1;
}

int
main(void)
{
    return test_batches_and_strings() || test_callback_failure()
           || test_admission_denial() || test_admission_release()
           || test_intern_denial_keeps_prior_ids();
}
