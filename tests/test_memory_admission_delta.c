/* test_memory_admission_delta.c - fixed delta-pool admission contract */

#include "../wirelog/columnar/delta_pool.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
            if (!(condition)) { \
                fprintf(stderr, "FAIL: %s\n", message); \
                failures++; \
            } \
} while (0)

static void
make_resolution(wl_columnar_memory_resolution_t *resolution,
    uint64_t usable_bytes)
{
    memset(resolution, 0, sizeof(*resolution));
    resolution->budget_bytes = usable_bytes;
    resolution->usable_bytes = usable_bytes;
    resolution->mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING;
    resolution->source = WL_COLUMNAR_MEMORY_SOURCE_ENV;
    resolution->status = WL_COLUMNAR_MEMORY_OK;
}

static void
test_managed_lifecycle(void)
{
    wl_columnar_memory_resolution_t resolution;
    wl_columnar_memory_governor_t governor;
    delta_pool_t *pool;

    make_resolution(&resolution, 200);
    CHECK(wl_columnar_memory_governor_init(&governor, &resolution)
        == WL_COLUMNAR_MEMORY_OK, "governor initialization");
    pool = delta_pool_create_managed(2, 9, 128, &governor);
    CHECK(pool != NULL, "managed pool creation");
    CHECK(wl_columnar_memory_reserved(&governor) == 160,
        "aligned slab plus data arena admitted");
    CHECK(delta_pool_alloc_slot(pool) != NULL, "slot allocation");
    CHECK(delta_pool_alloc_data(pool, 32) != NULL, "data allocation");
    delta_pool_reset(pool);
    CHECK(wl_columnar_memory_reserved(&governor) == 160,
        "reset retains fixed backing admission");
    delta_pool_destroy(pool);
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "destroy releases fixed backing admission");
}

static void
test_denial_overflow_and_legacy_contract(void)
{
    wl_columnar_memory_resolution_t resolution;
    wl_columnar_memory_governor_t governor;
    delta_pool_t *pool;

    make_resolution(&resolution, 159);
    CHECK(wl_columnar_memory_governor_init(&governor, &resolution)
        == WL_COLUMNAR_MEMORY_OK, "denial governor initialization");
    pool = delta_pool_create_managed(2, 9, 128, &governor);
    CHECK(pool == NULL, "denied pool footprint returns NULL");
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "denial leaves no reservation behind");
    pool = delta_pool_create_managed(1, SIZE_MAX, 1, &governor);
    CHECK(pool == NULL, "overflowing slot alignment returns NULL");
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "overflow leaves no reservation behind");
    pool = delta_pool_create_managed(1, 8, 64, NULL);
    CHECK(pool != NULL, "managed constructor preserves unmanaged mode");
    CHECK(delta_pool_alloc_data(pool, SIZE_MAX) == NULL,
        "overflowing data alignment returns NULL");
    delta_pool_destroy(pool);

    pool = delta_pool_create(1, 8, 64);
    CHECK(pool != NULL, "legacy unmanaged pool creation");
    CHECK(pool->admission_context == NULL
        && pool->admission_release == NULL,
        "legacy pool remains unmanaged");
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "legacy pool does not charge a governor");
    delta_pool_destroy(pool);
}

int
main(void)
{
    test_managed_lifecycle();
    test_denial_overflow_and_legacy_contract();
    if (failures != 0)
        return 1;
    puts("memory admission delta pool: PASS");
    return 0;
}
