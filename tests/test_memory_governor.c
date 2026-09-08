/*
 * test_memory_governor.c - deterministic memory resolver/governor tests
 *
 * Issue #1368: the governor is a foundation only. Allocation-site enforcement
 * and session ownership integration are separate implementation units.
 */

#include "../wirelog/columnar/memory_governor.h"
#include "wirelog/thread.h"

#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define TEST(name) do { tests_run++; printf("  [%d] %s", tests_run, name); \
} while (0)
#define PASS() do { printf(" ... PASS\n"); } while (0)
#define FAIL(msg) do { printf(" ... FAIL: %s\n", msg); tests_failed++; \
} while (0)

static int
test_explicit_values(void)
{
    wl_columnar_memory_resolution_t result;
    const char *invalid[] = {
        "", "0", "+268435456", "-1", " 268435456", "268435456 ",
        "abc", "268435455",
    };
    size_t i;

    TEST("strict explicit budget parsing");
    if (wl_columnar_memory_resolve("268435456", NULL, &result) != 0
        || result.mode != WL_COLUMNAR_MEMORY_MODE_ENFORCING
        || result.source != WL_COLUMNAR_MEMORY_SOURCE_ENV
        || result.budget_bytes != 268435456
        || result.headroom_bytes != 13421772
        || result.usable_bytes != 255013684) {
        FAIL("valid decimal budget or headroom arithmetic is wrong");
        return 1;
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (wl_columnar_memory_resolve(invalid[i], NULL, &result) == 0) {
            FAIL("invalid explicit budget was accepted");
            return 1;
        }
    }
    if (wl_columnar_memory_resolve("18446744073709551616", NULL, &result)
        != WL_COLUMNAR_MEMORY_OVERFLOW
        || result.status != WL_COLUMNAR_MEMORY_OVERFLOW) {
        FAIL("decimal overflow was not reported distinctly");
        return 1;
    }
    PASS();
    return 0;
}

static int
test_automatic_sources(void)
{
    wl_columnar_memory_sources_t sources;
    wl_columnar_memory_resolution_t result;

    TEST("automatic source reduction and advisory fallback");
    memset(&sources, 0, sizeof(sources));
    sources.cgroup_v2_available = true;
    sources.cgroup_v2_limit = UINT64_C(1024) * 1024 * 1024;
    sources.cgroup_v1_available = true;
    sources.cgroup_v1_limit = UINT64_C(2) * 1024 * 1024 * 1024;
    sources.rlimit_as_available = true;
    sources.rlimit_as_limit = UINT64_C(512) * 1024 * 1024;
    if (wl_columnar_memory_resolve(NULL, &sources, &result) != 0
        || result.source != WL_COLUMNAR_MEMORY_SOURCE_RLIMIT_AS
        || result.budget_bytes != sources.rlimit_as_limit
        || result.mode != WL_COLUMNAR_MEMORY_MODE_ENFORCING) {
        FAIL("effective automatic source was not selected");
        return 1;
    }
    sources.rlimit_as_limit = WL_COLUMNAR_MEMORY_MIN_BUDGET - 1;
    if (wl_columnar_memory_resolve(NULL, &sources, &result)
        != WL_COLUMNAR_MEMORY_INVALID) {
        FAIL("automatic budget below the minimum was accepted");
        return 1;
    }
    sources.rlimit_as_available = false;
    sources.rlimit_as_limit = 0;
    sources.cgroup_v2_available = false;
    sources.cgroup_v1_available = true;
    sources.cgroup_v1_limit = UINT64_C(1) << 62;
    if (wl_columnar_memory_resolve(NULL, &sources, &result)
        != WL_COLUMNAR_MEMORY_OK
        || result.mode != WL_COLUMNAR_MEMORY_MODE_ADVISORY) {
        FAIL("cgroup v1 unlimited sentinel was treated as a budget");
        return 1;
    }
    memset(&sources, 0, sizeof(sources));
    sources.windows_job_available = true;
    sources.windows_job_limit = UINT64_C(512) * 1024 * 1024;
    if (wl_columnar_memory_probe_windows_job(NULL, &sources)
        != WL_COLUMNAR_MEMORY_UNAVAILABLE
        || sources.windows_job_available || sources.windows_job_limit != 0) {
        FAIL("failed Windows provider probe retained stale limit");
        return 1;
    }
    if (wl_columnar_memory_resolve(NULL, &sources, &result) != 0
        || result.mode != WL_COLUMNAR_MEMORY_MODE_ADVISORY
        || result.usable_bytes != UINT64_MAX
        || result.source != WL_COLUMNAR_MEMORY_SOURCE_NONE) {
        FAIL("missing automatic source did not select advisory mode");
        return 1;
    }
    PASS();
    return 0;
}

static int
test_headroom_boundaries(void)
{
    wl_columnar_memory_sources_t sources;
    wl_columnar_memory_resolution_t result;

    TEST("headroom cap and small-budget arithmetic");
    memset(&sources, 0, sizeof(sources));
    sources.cgroup_v2_available = true;
    sources.cgroup_v2_limit = UINT64_C(16) * 1024 * 1024 * 1024;
    if (wl_columnar_memory_resolve(NULL, &sources, &result) != 0
        || result.headroom_bytes != WL_COLUMNAR_MEMORY_HEADROOM_CAP
        || result.usable_bytes
        != result.budget_bytes - WL_COLUMNAR_MEMORY_HEADROOM_CAP) {
        FAIL("headroom cap was not applied");
        return 1;
    }
    sources.cgroup_v2_limit = WL_COLUMNAR_MEMORY_MIN_BUDGET;
    if (wl_columnar_memory_resolve(NULL, &sources, &result) != 0
        || result.headroom_bytes != WL_COLUMNAR_MEMORY_MIN_BUDGET / 20
        || result.usable_bytes != result.budget_bytes - result.headroom_bytes) {
        FAIL("small-budget headroom underflow or rounding");
        return 1;
    }
    PASS();
    return 0;
}

static void
make_resolution(wl_columnar_memory_resolution_t *resolution,
    uint64_t budget, uint64_t usable)
{
    memset(resolution, 0, sizeof(*resolution));
    resolution->budget_bytes = budget;
    resolution->headroom_bytes = budget - usable;
    resolution->usable_bytes = usable;
    resolution->mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING;
    resolution->source = WL_COLUMNAR_MEMORY_SOURCE_ENV;
    resolution->status = WL_COLUMNAR_MEMORY_OK;
}

static int
test_reservation_lifecycle(void)
{
    wl_columnar_memory_resolution_t resolution;
    wl_columnar_memory_governor_t governor;
    wl_columnar_memory_reservation_t first;
    wl_columnar_memory_reservation_t second;
    int owner_a;
    int owner_b;

    TEST("reservation token lifecycle and denial");
    wl_columnar_memory_reservation_init(&first);
    wl_columnar_memory_reservation_init(&second);
    make_resolution(&resolution, 1000, 900);
    if (wl_columnar_memory_governor_init(&governor, &resolution) != 0
        || !wl_columnar_memory_reserve(&governor, 600, &first)
        || wl_columnar_memory_reserved(&governor) != 600
        || wl_columnar_memory_reserve(&governor, 301, &second)
        || wl_columnar_memory_reserved(&governor) != 600
        || !wl_columnar_memory_commit(&first, &owner_a)
        || !wl_columnar_memory_transfer(&first, &owner_b)
        || wl_columnar_memory_rollback(&first)
        || !wl_columnar_memory_release(&first)
        || wl_columnar_memory_release(&first)
        || wl_columnar_memory_reserved(&governor) != 0) {
        FAIL("commit/transfer/release lifecycle is not idempotent");
        return 1;
    }
    if (!wl_columnar_memory_reserve(&governor, 900, &second)
        || !wl_columnar_memory_rollback(&second)
        || wl_columnar_memory_rollback(&second)
        || wl_columnar_memory_reserved(&governor) != 0) {
        FAIL("rollback lifecycle is not enforced");
        return 1;
    }
    PASS();
    return 0;
}

struct reserve_thread_arg {
    wl_columnar_memory_governor_t *governor;
    wl_columnar_memory_reservation_t *reservation;
    bool admitted;
};

static void *
reserve_thread(void *arg)
{
    struct reserve_thread_arg *item = arg;
    item->admitted = wl_columnar_memory_reserve(item->governor, 200,
            item->reservation);
    return NULL;
}

static int
test_concurrent_admission(void)
{
    enum { THREADS = 8 };
    wl_columnar_memory_resolution_t resolution;
    wl_columnar_memory_governor_t governor;
    wl_columnar_memory_reservation_t reservations[THREADS];
    struct reserve_thread_arg args[THREADS];
    thread_t threads[THREADS];
    int admitted = 0;
    int i;

    TEST("concurrent reservations share one limit");
    make_resolution(&resolution, 1000, 900);
    if (wl_columnar_memory_governor_init(&governor, &resolution) != 0)
        return 1;
    for (i = 0; i < THREADS; i++) {
        wl_columnar_memory_reservation_init(&reservations[i]);
        args[i].governor = &governor;
        args[i].reservation = &reservations[i];
        args[i].admitted = false;
        if (thread_create(&threads[i], reserve_thread, &args[i]) != 0) {
            FAIL("thread creation failed");
            return 1;
        }
    }
    for (i = 0; i < THREADS; i++)
        thread_join(&threads[i]);
    for (i = 0; i < THREADS; i++)
        admitted += args[i].admitted ? 1 : 0;
    if (admitted > 4 || wl_columnar_memory_reserved(&governor) > 900) {
        FAIL("concurrent admission exceeded usable limit");
        return 1;
    }
    for (i = 0; i < THREADS; i++)
        if (args[i].admitted)
            wl_columnar_memory_release(&reservations[i]);
    if (wl_columnar_memory_reserved(&governor) != 0) {
        FAIL("concurrent reservation cleanup leaked capacity");
        return 1;
    }
    PASS();
    return 0;
}

int
main(void)
{
    printf("Memory Governor Unit Tests (Issue #1368)\n");
    printf("=========================================\n");
    test_explicit_values();
    test_automatic_sources();
    test_headroom_boundaries();
    test_reservation_lifecycle();
    test_concurrent_admission();
    printf("\nPassed %d/%d; Failed %d\n",
        tests_run - tests_failed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
