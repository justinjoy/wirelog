/* Program-owned intern-table admission tests for issue #1431. */

#include "../wirelog/columnar/memory_governor.h"
#include "../wirelog/intern.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>

static wl_columnar_memory_governor_ref_t *
test_governor(uint64_t usable_bytes)
{
    wl_columnar_memory_resolution_t resolution = {
        .budget_bytes = usable_bytes,
        .headroom_bytes = 0,
        .usable_bytes = usable_bytes,
        .mode = WL_COLUMNAR_MEMORY_MODE_ENFORCING,
        .source = WL_COLUMNAR_MEMORY_SOURCE_ENV,
        .status = WL_COLUMNAR_MEMORY_OK,
    };
    return wl_columnar_memory_governor_ref_create(&resolution);
}

static int
test_attach_and_release(void)
{
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *ref = test_governor(256);
    wl_columnar_memory_governor_t *governor;
    int rc;

    if (!intern || !ref)
        return 1;
    governor = wl_columnar_memory_governor_ref_get(ref);
    rc = wl_intern_attach_memory_governor(intern, ref);
    if (rc != 0 || wl_columnar_memory_reserved(governor) != 256) {
        wl_intern_free(intern);
        wl_columnar_memory_governor_ref_release(ref);
        return 1;
    }
    wl_intern_free(intern);
    if (wl_columnar_memory_reserved(governor) != 0) {
        wl_columnar_memory_governor_ref_release(ref);
        return 1;
    }
    wl_columnar_memory_governor_ref_release(ref);
    return 0;
}

static int
test_denied_growth_preserves_table(void)
{
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *ref = test_governor(256);
    wl_columnar_memory_governor_t *governor;
    int64_t id;

    if (!intern || !ref)
        return 1;
    governor = wl_columnar_memory_governor_ref_get(ref);
    if (wl_intern_attach_memory_governor(intern, ref) != 0)
        return 1;
    id = wl_intern_put(intern, "new-symbol");
    if (id != -1 || wl_intern_count(intern) != 0
        || wl_intern_get(intern, "new-symbol") != -1
        || wl_columnar_memory_reserved(governor) != 256) {
        wl_intern_free(intern);
        wl_columnar_memory_governor_ref_release(ref);
        return 1;
    }
    wl_intern_free(intern);
    wl_columnar_memory_governor_ref_release(ref);
    return 0;
}

static int
test_duplicate_is_uncharged(void)
{
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *ref = test_governor(4096);
    wl_columnar_memory_governor_t *governor;
    uint64_t before;
    int64_t first;
    int64_t duplicate;

    if (!intern || !ref)
        return 1;
    governor = wl_columnar_memory_governor_ref_get(ref);
    if (wl_intern_attach_memory_governor(intern, ref) != 0)
        return 1;
    first = wl_intern_put(intern, "same-symbol");
    before = wl_columnar_memory_reserved(governor);
    duplicate = wl_intern_put(intern, "same-symbol");
    if (first < 0 || duplicate != first
        || wl_columnar_memory_reserved(governor) != before) {
        wl_intern_free(intern);
        wl_columnar_memory_governor_ref_release(ref);
        return 1;
    }
    wl_intern_free(intern);
    wl_columnar_memory_governor_ref_release(ref);
    return 0;
}

static int
test_conflicting_governor_is_rejected(void)
{
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *first = test_governor(4096);
    wl_columnar_memory_governor_ref_t *second = test_governor(4096);
    int rc;

    if (!intern || !first || !second)
        return 1;
    if (wl_intern_attach_memory_governor(intern, first) != 0)
        return 1;
    rc = wl_intern_attach_memory_governor(intern, second);
    wl_intern_free(intern);
    wl_columnar_memory_governor_ref_release(first);
    wl_columnar_memory_governor_ref_release(second);
    return rc == EBUSY ? 0 : 1;
}

static int
test_same_governor_is_idempotent_and_detachable(void)
{
    wl_intern_t *intern = wl_intern_create();
    wl_columnar_memory_governor_ref_t *ref = test_governor(4096);
    wl_columnar_memory_governor_t *governor;
    int rc;

    if (!intern || !ref)
        return 1;
    governor = wl_columnar_memory_governor_ref_get(ref);
    if (wl_intern_attach_memory_governor(intern, ref) != 0)
        return 1;
    rc = wl_intern_attach_memory_governor(intern, ref);
    if (rc != EALREADY || wl_columnar_memory_reserved(governor) == 0)
        return 1;
    if (wl_intern_detach_memory_governor(intern, ref) != 0
        || wl_columnar_memory_reserved(governor) != 0) {
        wl_intern_free(intern);
        wl_columnar_memory_governor_ref_release(ref);
        return 1;
    }
    wl_intern_free(intern);
    wl_columnar_memory_governor_ref_release(ref);
    return 0;
}

int
main(void)
{
    int failures = 0;
    failures += test_attach_and_release();
    failures += test_denied_growth_preserves_table();
    failures += test_duplicate_is_uncharged();
    failures += test_conflicting_governor_is_rejected();
    failures += test_same_governor_is_idempotent_and_detachable();
    if (failures != 0) {
        fprintf(stderr, "memory admission intern tests failed: %d\n",
            failures);
        return 1;
    }
    puts("memory admission intern tests passed");
    return 0;
}
