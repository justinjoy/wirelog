/*
 * test_memory_admission_arena.c - fixed arena admission contract
 *
 * The arena owns one reservation for its fixed backing slab.  Reset retains
 * the slab and therefore retains admission; destroy is the only terminal
 * release path.
 */

#include "../wirelog/arena/arena.h"
#include "../wirelog/columnar/memory_governor.h"

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
    wl_arena_t *arena;

    make_resolution(&resolution, 1024);
    CHECK(wl_columnar_memory_governor_init(&governor, &resolution)
        == WL_COLUMNAR_MEMORY_OK, "governor initialization");

    arena = wl_arena_create_managed(512, &governor);
    CHECK(arena != NULL, "managed arena creation");
    CHECK(wl_columnar_memory_reserved(&governor) == 512,
        "fixed backing capacity admitted");
    CHECK(wl_arena_alloc(arena, 64) != NULL, "managed arena allocation");

    wl_arena_reset(arena);
    CHECK(wl_columnar_memory_reserved(&governor) == 512,
        "reset retains backing admission");
    CHECK(wl_arena_alloc(arena, 512) != NULL,
        "reset makes the admitted slab reusable");
    wl_arena_free(arena);
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "destroy releases backing admission");
}

static void
test_denial_and_legacy_contract(void)
{
    wl_columnar_memory_resolution_t resolution;
    wl_columnar_memory_governor_t governor;
    wl_arena_t *arena;

    make_resolution(&resolution, 512);
    CHECK(wl_columnar_memory_governor_init(&governor, &resolution)
        == WL_COLUMNAR_MEMORY_OK, "denial governor initialization");
    arena = wl_arena_create_managed(513, &governor);
    CHECK(arena == NULL, "denied backing capacity returns NULL");
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "denial leaves no reservation behind");

    arena = wl_arena_create(64);
    CHECK(arena != NULL, "legacy unmanaged arena creation");
    CHECK(arena->admission_context == NULL
        && arena->admission_release == NULL,
        "legacy arena remains unmanaged");
    CHECK(wl_columnar_memory_reserved(&governor) == 0,
        "legacy arena does not charge a governor");
    wl_arena_free(arena);
}

int
main(void)
{
    test_managed_lifecycle();
    test_denial_and_legacy_contract();
    if (failures != 0)
        return 1;
    puts("memory admission arena: PASS");
    return 0;
}
