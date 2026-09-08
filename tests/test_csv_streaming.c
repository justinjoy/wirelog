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

int
main(void)
{
    return test_batches_and_strings() || test_callback_failure()
           || test_admission_denial() || test_admission_release();
}
