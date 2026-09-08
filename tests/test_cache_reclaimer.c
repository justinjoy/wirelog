/*
 * test_cache_reclaimer.c - pin/generation-safe materialization cache reclaim
 *
 * Issue #1371: the reclaimer must release only cache-owned capacity at a
 * quiescent point and must remain safe across pinning, stale generations, and
 * owner teardown.
 */

#include "columnar/internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static col_rel_t *
make_relation(int64_t value)
{
    col_rel_t *rel = col_rel_new_auto("cache_reclaim", 1);
    assert(rel != NULL);
    int64_t row[] = { value };
    assert(col_rel_append_row(rel, row) == 0);
    return rel;
}

static wl_mem_reclaim_result_t
count_callback(void *owner)
{
    uint32_t *calls = (uint32_t *)owner;
    (*calls)++;
    wl_mem_reclaim_result_t result = { 0, 0 };
    return result;
}

static void
test_unregister_during_teardown(void)
{
    wl_mem_ledger_t ledger;
    wl_mem_ledger_init(&ledger, 1);
    uint32_t calls = 0;
    wl_mem_reclaimer_handle_t handle = 0;

    assert(wl_mem_ledger_register_reclaimer(&ledger, count_callback, &calls,
        &handle) == 0);
    assert(handle != 0);
    wl_mem_ledger_unregister_reclaimer(&ledger, handle);
    wl_mem_reclaim_result_t result = wl_mem_ledger_reclaim(&ledger);
    assert(result.bytes_released == 0 && result.candidates == 0);
    assert(calls == 0);
}

static void
test_pin_generation_and_idempotence(void)
{
    wl_mem_ledger_t ledger;
    wl_mem_ledger_init(&ledger, 0);
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.ledger = &ledger;
    assert(col_mat_cache_attach_reclaimer(&cache) == 0);

    col_rel_t *result = make_relation(7);
    assert(col_mat_cache_insert(&cache, result, result, result) == 0);
    assert(cache.count == 1);
    assert(cache.entries[0].owns_result);
    uint64_t capacity = col_rel_transport_bytes(cache.entries[0].result);
    assert(capacity > 0);

    assert(col_mat_cache_lookup(&cache, result, result) == result);
    assert(cache.entries[0].pin_count == 1);
    /* Repeated hits in one evaluation epoch take only one pin. */
    assert(col_mat_cache_lookup(&cache, result, result) == result);
    assert(cache.entries[0].pin_count == 1);
    wl_mem_reclaim_result_t reclaim = wl_mem_ledger_reclaim(&ledger);
    assert(reclaim.bytes_released == 0 && reclaim.candidates == 0);
    assert(cache.count == 1);

    col_mat_cache_release_pins(&cache);
    assert(cache.entries[0].pin_count == 0);
    reclaim = wl_mem_ledger_reclaim(&ledger);
    assert(reclaim.bytes_released == capacity);
    assert(reclaim.candidates == 1 && cache.count == 0);

    reclaim = wl_mem_ledger_reclaim(&ledger);
    assert(reclaim.bytes_released == 0 && reclaim.candidates == 0);
    col_mat_cache_detach_reclaimer(&cache);
}

static void
test_generation_mismatch(void)
{
    wl_mem_ledger_t ledger;
    wl_mem_ledger_init(&ledger, 0);
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.ledger = &ledger;
    col_rel_t *result = make_relation(11);
    assert(col_mat_cache_insert(&cache, result, result, result) == 0);
    uint64_t generation = cache.entries[0].generation;

    wl_mem_reclaim_result_t reclaim
        = col_mat_cache_reclaim_entry(&cache, 0, generation + 1);
    assert(reclaim.bytes_released == 0 && reclaim.candidates == 0);
    assert(cache.count == 1);

    reclaim = col_mat_cache_reclaim_entry(&cache, 0, generation);
    assert(reclaim.bytes_released > 0 && reclaim.candidates == 1);
    assert(cache.count == 0);
}

static void
test_shared_storage_is_not_reclaimable(void)
{
    wl_mem_ledger_t ledger;
    wl_mem_ledger_init(&ledger, 0);
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.ledger = &ledger;
    assert(col_mat_cache_attach_reclaimer(&cache) == 0);

    col_rel_t *source = make_relation(19);
    col_rel_t *view = col_rel_new_like("cache_view", source);
    assert(view != NULL);
    assert(col_rel_install_shared_view(view, source) == 0);
    assert(col_mat_cache_insert(&cache, source, source, view) == 0);
    assert(cache.count == 1);
    assert(!cache.entries[0].owns_result);

    wl_mem_reclaim_result_t reclaim = wl_mem_ledger_reclaim(&ledger);
    assert(reclaim.bytes_released == 0 && reclaim.candidates == 0);
    assert(cache.count == 1);

    col_mat_cache_detach_reclaimer(&cache);
    col_mat_cache_clear(&cache);
    col_rel_destroy(source);
}

static void
test_lru_evicts_oldest_unpinned_entry(void)
{
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));

    col_rel_t *oldest = make_relation(1);
    col_rel_t *newest = make_relation(2);
    assert(col_mat_cache_insert(&cache, oldest, oldest, oldest) == 0);
    assert(col_mat_cache_insert(&cache, newest, newest, newest) == 0);
    assert(cache.count == 2);
    size_t before = cache.total_bytes;

    col_mat_cache_evict_until(&cache, before);
    assert(cache.count == 1);
    assert(col_rel_get(cache.entries[0].result, 0, 0) == 2);

    col_mat_cache_clear(&cache);
}

static void
test_quiescent_step_release_makes_entry_evictable(void)
{
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));

    col_rel_t *result = make_relation(3);
    assert(col_mat_cache_insert(&cache, result, result, result) == 0);
    assert(col_mat_cache_lookup(&cache, result, result) == result);
    assert(cache.entries[0].pin_count == 1);

    /* Model the quiescent boundary used after a session step/sub-pass. */
    col_mat_cache_release_pins(&cache);
    assert(cache.entries[0].pin_count == 0);
    col_mat_cache_evict_until(&cache, 0);
    assert(cache.count == 0);
}

static void
test_pinned_insert_preserves_caller_ownership(void)
{
    col_mat_cache_t cache;
    memset(&cache, 0, sizeof(cache));

    col_rel_t *existing[COL_MAT_CACHE_MAX];
    for (uint32_t i = 0; i < COL_MAT_CACHE_MAX; i++) {
        existing[i] = make_relation((int64_t)i);
        assert(col_mat_cache_insert(&cache, existing[i], existing[i],
            existing[i]) == 0);
        cache.entries[i].pin_count = 1;
    }

    col_rel_t *result = make_relation(1000);
    assert(col_mat_cache_insert(&cache, result, result, result) == ENOSPC);
    assert(cache.count == COL_MAT_CACHE_MAX);
    assert(result->nrows == 1 && col_rel_get(result, 0, 0) == 1000);

    for (uint32_t i = 0; i < cache.count; i++)
        cache.entries[i].pin_count = 0;
    col_mat_cache_clear(&cache);
    col_rel_destroy(result);
}

int
main(void)
{
    test_unregister_during_teardown();
    test_pin_generation_and_idempotence();
    test_generation_mismatch();
    test_shared_storage_is_not_reclaimable();
    test_lru_evicts_oldest_unpinned_entry();
    test_quiescent_step_release_makes_entry_evictable();
    test_pinned_insert_preserves_caller_ownership();
    puts("cache reclaimer tests passed");
    return 0;
}
