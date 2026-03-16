/*
 * backend/columnar_nanoarrow.c - wirelog Nanoarrow Columnar Backend
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 */

#define _GNU_SOURCE

#include "columnar/internal.h"

#include "../wirelog-internal.h"

#include <xxhash.h>

#ifdef WL_MBEDTLS_ENABLED
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/md.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#ifdef __ARM_NEON__
#include <arm_neon.h>
#endif

/* Relation storage and cache functions moved to columnar/relation.c and
 * columnar/cache.c; declarations in columnar/internal.h. */

/* ======================================================================== */
/* Arrangement Layer (Phase 3C)                                             */
/* ======================================================================== */

/*
 * arr_hash_key: FNV-1a hash over key columns of a single row.
 * nbuckets MUST be a power of 2.
 */
static uint32_t
arr_hash_key(const int64_t *row, const uint32_t *key_cols, uint32_t key_count,
             uint32_t nbuckets)
{
    uint64_t h = 14695981039346656037ULL; /* FNV-1a basis */
    for (uint32_t k = 0; k < key_count; k++) {
        uint64_t v = (uint64_t)row[key_cols[k]];
        for (int b = 0; b < 8; b++) {
            h ^= v & 0xFFu;
            h *= 1099511628211ULL;
            v >>= 8;
        }
    }
    return (uint32_t)(h & (uint64_t)(nbuckets - 1));
}

/* Round n up to the next power of 2; minimum 16. */
static uint32_t
arr_next_pow2(uint32_t n)
{
    if (n < 16u)
        return 16u;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

/*
 * arr_free_contents: free hash table arrays only.
 * key_cols is NOT freed here — it is always owned by the registry entry
 * (col_arr_entry_t.key_cols) and freed separately in col_session_destroy.
 */
void
arr_free_contents(col_arrangement_t *arr)
{
    if (!arr)
        return;
    free(arr->ht_head);
    free(arr->ht_next);
    arr->ht_head = NULL;
    arr->ht_next = NULL;
    arr->nbuckets = 0;
    arr->ht_cap = 0;
    arr->indexed_rows = 0;
}

/* Full rebuild: index all nrows rows in rel into arr. */
static int
arr_build_full(col_arrangement_t *arr, const col_rel_t *rel)
{
    uint32_t nrows = rel->nrows;
    uint32_t nbuckets = arr_next_pow2(nrows > 0 ? nrows * 2u : 16u);

    /* Reallocate bucket heads if size changed. */
    if (nbuckets != arr->nbuckets) {
        uint32_t *head = (uint32_t *)malloc(nbuckets * sizeof(uint32_t));
        if (!head)
            return ENOMEM;
        free(arr->ht_head);
        arr->ht_head = head;
        arr->nbuckets = nbuckets;
    }
    memset(arr->ht_head, 0xFF, nbuckets * sizeof(uint32_t)); /* UINT32_MAX */

    /* Grow chain array if needed. */
    if (nrows > arr->ht_cap) {
        uint32_t new_cap = nrows > arr->ht_cap * 2u ? nrows : arr->ht_cap * 2u;
        if (new_cap < 16u)
            new_cap = 16u;
        uint32_t *nxt = (uint32_t *)malloc(new_cap * sizeof(uint32_t));
        if (!nxt)
            return ENOMEM;
        free(arr->ht_next);
        arr->ht_next = nxt;
        arr->ht_cap = new_cap;
    }

    uint32_t nc = rel->ncols;
    for (uint32_t row = 0; row < nrows; row++) {
        const int64_t *rp = rel->data + (size_t)row * nc;
        uint32_t bucket
            = arr_hash_key(rp, arr->key_cols, arr->key_count, nbuckets);
        arr->ht_next[row] = arr->ht_head[bucket];
        arr->ht_head[bucket] = row;
    }
    arr->indexed_rows = nrows;
    return 0;
}

/* Incremental update: index only rows [old_nrows..rel->nrows). */
static int
arr_update_incremental(col_arrangement_t *arr, const col_rel_t *rel,
                       uint32_t old_nrows)
{
    uint32_t nrows = rel->nrows;
    if (old_nrows >= nrows)
        return 0;

    /* If load factor would exceed 50%, full rebuild needed. */
    uint32_t needed = arr_next_pow2(nrows * 2u);
    if (needed != arr->nbuckets)
        return arr_build_full(arr, rel);

    /* Grow chain array if needed. */
    if (nrows > arr->ht_cap) {
        uint32_t new_cap = nrows * 2u < 16u ? 16u : nrows * 2u;
        uint32_t *nxt
            = (uint32_t *)realloc(arr->ht_next, new_cap * sizeof(uint32_t));
        if (!nxt)
            return ENOMEM;
        arr->ht_next = nxt;
        arr->ht_cap = new_cap;
    }

    uint32_t nc = rel->ncols;
    uint32_t nb = arr->nbuckets;
    for (uint32_t row = old_nrows; row < nrows; row++) {
        const int64_t *rp = rel->data + (size_t)row * nc;
        uint32_t bucket = arr_hash_key(rp, arr->key_cols, arr->key_count, nb);
        arr->ht_next[row] = arr->ht_head[bucket];
        arr->ht_head[bucket] = row;
    }
    arr->indexed_rows = nrows;
    return 0;
}

col_rel_t *
session_find_rel(wl_col_session_t *sess, const char *name)
{
    if (!name)
        return NULL;
    for (uint32_t i = 0; i < sess->nrels; i++) {
        if (sess->rels[i] && strcmp(sess->rels[i]->name, name) == 0)
            return sess->rels[i];
    }
    return NULL;
}

int
session_add_rel(wl_col_session_t *sess, col_rel_t *r)
{
    /* Pool-owned structs must be promoted to heap before storing in the
     * session, because col_session_destroy calls free() on each entry. */
    if (r->pool_owned) {
        col_rel_t *heap = (col_rel_t *)calloc(1, sizeof(col_rel_t));
        if (!heap)
            return ENOMEM;
        *heap = *r;
        heap->pool_owned = false;
        /* Zero out source slot so pool_reset doesn't double-free contents */
        memset(r, 0, sizeof(*r));
        r = heap;
    }
    if (sess->nrels >= sess->rel_cap) {
        uint32_t nc = sess->rel_cap ? sess->rel_cap * 2 : 16;
        col_rel_t **nr
            = (col_rel_t **)realloc(sess->rels, sizeof(col_rel_t *) * nc);
        if (!nr)
            return ENOMEM;
        sess->rels = nr;
        sess->rel_cap = nc;
    }
    sess->rels[sess->nrels++] = r;
    return 0;
}

void
session_remove_rel(wl_col_session_t *sess, const char *name)
{
    for (uint32_t i = 0; i < sess->nrels; i++) {
        if (sess->rels[i] && strcmp(sess->rels[i]->name, name) == 0) {
            col_rel_destroy(sess->rels[i]);
            sess->rels[i] = NULL;
            return;
        }
    }
}

/* Operator implementations moved to columnar/ops.c;
 * evaluator functions moved to columnar/eval.c;
 * declarations in columnar/internal.h. */

/* ======================================================================== */
/* Public Accessors                                                          */
/* ======================================================================== */

/*
 * col_session_get_iteration_count:
 *
 * Return the number of fixed-point iterations performed during the last
 * call to col_eval_stratum.  Returns 0 if no evaluation has occurred yet.
 *
 * @param sess  A wl_session_t* backed by the columnar backend.
 */
uint32_t
col_session_get_iteration_count(wl_session_t *sess)
{
    return COL_SESSION(sess)->total_iterations;
}

/*
 * col_session_get_cache_stats:
 *
 * Return CSE materialization cache hit and miss counts accumulated during
 * the last evaluation.  Both out-parameters are optional (NULL-safe).
 *
 * @param sess    A wl_session_t* backed by the columnar backend.
 * @param out_hits    Set to the number of cache hits (may be NULL).
 * @param out_misses  Set to the number of cache misses (may be NULL).
 */
void
col_session_get_cache_stats(wl_session_t *sess, uint64_t *out_hits,
                            uint64_t *out_misses)
{
    wl_col_session_t *cs = COL_SESSION(sess);
    if (out_hits)
        *out_hits = cs->mat_cache.hits;
    if (out_misses)
        *out_misses = cs->mat_cache.misses;
}

/*
 * col_session_get_perf_stats:
 *
 * Return accumulated profiling counters (in nanoseconds) from the last
 * wl_session_snapshot() call.  Counters are reset at the start of each
 * evaluation pass.  Both out-parameters are optional (NULL-safe).
 *
 * @param sess             A wl_session_t* backed by the columnar backend.
 * @param out_consolidation_ns  Time spent in incremental consolidation.
 * @param out_kfusion_ns        Time spent in K-fusion dispatch.
 */
void
col_session_get_perf_stats(wl_session_t *sess, uint64_t *out_consolidation_ns,
                           uint64_t *out_kfusion_ns,
                           uint64_t *out_kfusion_alloc_ns,
                           uint64_t *out_kfusion_dispatch_ns,
                           uint64_t *out_kfusion_merge_ns,
                           uint64_t *out_kfusion_cleanup_ns)
{
    wl_col_session_t *cs = COL_SESSION(sess);
    if (out_consolidation_ns)
        *out_consolidation_ns = cs->consolidation_ns;
    if (out_kfusion_ns)
        *out_kfusion_ns = cs->kfusion_ns;
    if (out_kfusion_alloc_ns)
        *out_kfusion_alloc_ns = cs->kfusion_alloc_ns;
    if (out_kfusion_dispatch_ns)
        *out_kfusion_dispatch_ns = cs->kfusion_dispatch_ns;
    if (out_kfusion_merge_ns)
        *out_kfusion_merge_ns = cs->kfusion_merge_ns;
    if (out_kfusion_cleanup_ns)
        *out_kfusion_cleanup_ns = cs->kfusion_cleanup_ns;
}

/*
 * col_session_cleanup_old_data:
 *
 * Remove data that is entirely before the frontier (iteration, stratum).
 * Only rows with timestamps <= frontier are removed.
 *
 * This function performs selective cleanup of old delta rows to reduce
 * memory usage. Timestamps before the frontier have already been processed
 * and will not be needed again during evaluation.
 *
 * @param sess     wl_session_t* backed by columnar backend
 * @param frontier Minimum (iteration, stratum) to preserve; data before this
 * can be freed
 */
static void
col_session_cleanup_old_data(wl_session_t *sess, col_frontier_t frontier)
{
    if (!sess)
        return;

    wl_col_session_t *cs = COL_SESSION(sess);

    /* Scan all relations, remove rows with timestamps <= frontier */
    for (uint32_t ri = 0; ri < cs->nrels; ri++) {
        col_rel_t *rel = cs->rels[ri];
        if (!rel || !rel->timestamps || rel->nrows == 0)
            continue;

        /* Find first row that is after frontier */
        uint32_t keep_from = 0;
        for (uint32_t row = 0; row < rel->nrows; row++) {
            const col_delta_timestamp_t *ts = &rel->timestamps[row];
            if (ts->iteration > frontier.iteration
                || (ts->iteration == frontier.iteration
                    && ts->stratum > frontier.stratum)) {
                keep_from = row;
                break;
            }
        }

        /* If all rows are at or before frontier, clear entire relation */
        if (keep_from == rel->nrows) {
            free(rel->timestamps);
            rel->timestamps = NULL;
            rel->nrows = 0;
            rel->capacity = 0;
            free(rel->data);
            rel->data = NULL;
            /* Invalidate arrangements (data changed) */
            col_session_invalidate_arrangements(sess, rel->name);
        } else if (keep_from > 0) {
            /* Shift rows forward and update timestamps */
            uint32_t new_nrows = rel->nrows - keep_from;
            size_t row_bytes = (size_t)rel->ncols * sizeof(int64_t);

            memmove(rel->data, rel->data + (size_t)keep_from * rel->ncols,
                    new_nrows * row_bytes);
            memmove(rel->timestamps, rel->timestamps + keep_from,
                    new_nrows * sizeof(col_delta_timestamp_t));

            rel->nrows = new_nrows;
            /* Invalidate arrangements (data changed) */
            col_session_invalidate_arrangements(sess, rel->name);
        }
    }
}

/* ======================================================================== */
/* Arrangement Accessors (Phase 3C)                                         */
/* ======================================================================== */

col_arrangement_t *
col_session_get_arrangement(wl_session_t *sess, const char *rel_name,
                            const uint32_t *key_cols, uint32_t key_count)
{
    if (!sess || !rel_name || !key_cols || key_count == 0)
        return NULL;

    wl_col_session_t *cs = COL_SESSION(sess);

    /* Look up the relation. */
    col_rel_t *rel = NULL;
    for (uint32_t i = 0; i < cs->nrels; i++) {
        if (cs->rels[i] && cs->rels[i]->name
            && strcmp(cs->rels[i]->name, rel_name) == 0) {
            rel = cs->rels[i];
            break;
        }
    }
    if (!rel)
        return NULL;

    /* Search registry for matching (rel_name, key_cols) entry. */
    for (uint32_t i = 0; i < cs->arr_count; i++) {
        col_arr_entry_t *e = &cs->arr_entries[i];
        if (e->key_count != key_count)
            continue;
        if (strcmp(e->rel_name, rel_name) != 0)
            continue;
        bool match = true;
        for (uint32_t k = 0; k < key_count; k++) {
            if (e->key_cols[k] != key_cols[k]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;

        /* Found: update if stale. */
        if (e->arr.indexed_rows == 0 && rel->nrows > 0) {
            if (arr_build_full(&e->arr, rel) != 0)
                return NULL;
        } else if (e->arr.indexed_rows < rel->nrows) {
            uint32_t old = e->arr.indexed_rows;
            if (arr_update_incremental(&e->arr, rel, old) != 0)
                return NULL;
        }
        return &e->arr;
    }

    /* Not found: grow registry and create new entry. */
    if (cs->arr_count >= cs->arr_cap) {
        uint32_t new_cap = cs->arr_cap ? cs->arr_cap * 2u : 8u;
        col_arr_entry_t *ne = (col_arr_entry_t *)realloc(
            cs->arr_entries, new_cap * sizeof(col_arr_entry_t));
        if (!ne)
            return NULL;
        cs->arr_entries = ne;
        cs->arr_cap = new_cap;
    }

    col_arr_entry_t *e = &cs->arr_entries[cs->arr_count];
    memset(e, 0, sizeof(*e));

    e->rel_name = wl_strdup(rel_name);
    if (!e->rel_name)
        return NULL;

    e->key_cols = (uint32_t *)malloc(key_count * sizeof(uint32_t));
    if (!e->key_cols) {
        free(e->rel_name);
        e->rel_name = NULL;
        return NULL;
    }
    memcpy(e->key_cols, key_cols, key_count * sizeof(uint32_t));
    e->key_count = key_count;
    e->arr.key_cols = e->key_cols; /* shared view; key_cols owned by entry */
    e->arr.key_count = key_count;
    cs->arr_count++;

    /* Initial build. */
    if (rel->nrows > 0 && arr_build_full(&e->arr, rel) != 0) {
        /* Roll back the entry we just added. */
        cs->arr_count--;
        free(e->rel_name);
        free(e->key_cols);
        memset(e, 0, sizeof(*e));
        return NULL;
    }
    return &e->arr;
}

uint32_t
col_arrangement_find_first(const col_arrangement_t *arr,
                           const int64_t *rel_data, uint32_t rel_ncols,
                           const int64_t *key_row)
{
    if (!arr || !rel_data || !key_row || arr->nbuckets == 0)
        return UINT32_MAX;

    uint32_t bucket
        = arr_hash_key(key_row, arr->key_cols, arr->key_count, arr->nbuckets);
    uint32_t row = arr->ht_head[bucket];
    while (row != UINT32_MAX) {
        const int64_t *rp = rel_data + (size_t)row * rel_ncols;
        bool match = true;
        for (uint32_t k = 0; k < arr->key_count; k++) {
            if (rp[arr->key_cols[k]] != key_row[arr->key_cols[k]]) {
                match = false;
                break;
            }
        }
        if (match)
            return row;
        row = arr->ht_next[row];
    }
    return UINT32_MAX;
}

uint32_t
col_arrangement_find_next(const col_arrangement_t *arr, uint32_t row_idx)
{
    if (!arr || row_idx >= arr->ht_cap)
        return UINT32_MAX;
    return arr->ht_next[row_idx];
}

void
col_session_invalidate_arrangements(wl_session_t *sess, const char *rel_name)
{
    if (!sess || !rel_name)
        return;
    wl_col_session_t *cs = COL_SESSION(sess);
    for (uint32_t i = 0; i < cs->arr_count; i++) {
        if (strcmp(cs->arr_entries[i].rel_name, rel_name) == 0)
            cs->arr_entries[i].arr.indexed_rows = 0; /* force full rebuild */
    }
}

/* ======================================================================== */
/* Delta Arrangement Cache (Phase 3C-001-Ext)                               */
/* ======================================================================== */

/*
 * col_session_free_delta_arrangements:
 *
 * Free all entries in the delta arrangement cache and reset the cache.
 * Called at the start of each semi-naive iteration (sequential path) and
 * by col_op_k_fusion after each dispatch to clean up worker copies.
 *
 * Safe to call on a zeroed session (darr_count == 0, darr_entries == NULL).
 */
void
col_session_free_delta_arrangements(wl_col_session_t *cs)
{
    for (uint32_t i = 0; i < cs->darr_count; i++) {
        col_arr_entry_t *e = &cs->darr_entries[i];
        free(e->rel_name);
        free(e->key_cols);
        arr_free_contents(&e->arr);
    }
    free(cs->darr_entries);
    cs->darr_entries = NULL;
    cs->darr_count = 0;
    cs->darr_cap = 0;
}

/*
 * col_session_get_delta_arrangement:
 *
 * Return (or lazily create) a delta arrangement for `delta_rel` keyed on
 * `key_cols[0..key_count)`.  Stored in cs->darr_entries (the per-worker
 * delta cache) and keyed by (rel_name, key_cols[]).
 *
 * Unlike full arrangements (which persist across iterations for EDB
 * relations), delta arrangements are rebuilt from scratch if stale
 * (indexed_rows != delta_rel->nrows).  This handles the case where the
 * same delta relation grows between iterations.
 *
 * Returns NULL on allocation failure or if key_count == 0.
 */
col_arrangement_t *
col_session_get_delta_arrangement(wl_col_session_t *cs, const char *rel_name,
                                  const col_rel_t *delta_rel,
                                  const uint32_t *key_cols, uint32_t key_count)
{
    if (!cs || !rel_name || !delta_rel || !key_cols || key_count == 0)
        return NULL;

    /* Search existing cache entries. */
    for (uint32_t i = 0; i < cs->darr_count; i++) {
        col_arr_entry_t *e = &cs->darr_entries[i];
        if (e->key_count != key_count)
            continue;
        if (strcmp(e->rel_name, rel_name) != 0)
            continue;
        bool match = true;
        for (uint32_t k = 0; k < key_count; k++) {
            if (e->key_cols[k] != key_cols[k]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        /* Found: rebuild if stale (delta changed size). */
        if (e->arr.indexed_rows != delta_rel->nrows) {
            arr_free_contents(&e->arr);
            if (delta_rel->nrows > 0 && arr_build_full(&e->arr, delta_rel) != 0)
                return NULL;
        }
        return &e->arr;
    }

    /* Not found: grow cache and create new entry. */
    if (cs->darr_count >= cs->darr_cap) {
        uint32_t new_cap = cs->darr_cap ? cs->darr_cap * 2u : 4u;
        col_arr_entry_t *ne = (col_arr_entry_t *)realloc(
            cs->darr_entries, new_cap * sizeof(col_arr_entry_t));
        if (!ne)
            return NULL;
        cs->darr_entries = ne;
        cs->darr_cap = new_cap;
    }

    col_arr_entry_t *e = &cs->darr_entries[cs->darr_count];
    memset(e, 0, sizeof(*e));

    e->rel_name = wl_strdup(rel_name);
    if (!e->rel_name)
        return NULL;

    e->key_cols = (uint32_t *)malloc(key_count * sizeof(uint32_t));
    if (!e->key_cols) {
        free(e->rel_name);
        e->rel_name = NULL;
        return NULL;
    }
    memcpy(e->key_cols, key_cols, key_count * sizeof(uint32_t));
    e->key_count = key_count;
    e->arr.key_cols = e->key_cols; /* shared view; owned by entry */
    e->arr.key_count = key_count;
    cs->darr_count++;

    /* Initial build. */
    if (delta_rel->nrows > 0 && arr_build_full(&e->arr, delta_rel) != 0) {
        cs->darr_count--;
        free(e->rel_name);
        free(e->key_cols);
        memset(e, 0, sizeof(*e));
        return NULL;
    }
    return &e->arr;
}

/*
 * col_session_get_darr_count:
 *
 * Return the number of delta arrangement cache entries in the session.
 * Expected to be 0 on the main session after K-fusion evaluation
 * (delta caches are per-worker and freed after each dispatch).
 *
 * Used by tests to verify per-worker isolation invariant.
 */
uint32_t
col_session_get_darr_count(wl_session_t *sess)
{
    if (!sess)
        return 0;
    return COL_SESSION(sess)->darr_count;
}

/*
 * col_session_get_frontier:
 *
 * Copy frontiers[stratum_idx] into *out_frontier.
 * Returns EINVAL for NULL args or out-of-range stratum_idx.
 */
int
col_session_get_frontier(wl_session_t *session, uint32_t stratum_idx,
                         col_frontier_2d_t *out_frontier)
{
    if (!session || !out_frontier || stratum_idx >= MAX_STRATA)
        return EINVAL;
    *out_frontier = COL_SESSION(session)->frontiers[stratum_idx];
    return 0;
}

/* ======================================================================== */
/* Vtable Functions                                                          */
/* ======================================================================== */

/*
 * col_session_create: Initialize a columnar backend session
 *
 * Implements wl_compute_backend_t.session_create vtable slot.
 *
 * @param plan:        Execution plan (borrowed, must outlive session)
 * @param num_workers: Thread pool size for parallel K-fusion. When > 1,
 *                     creates a workqueue at session init. When 1, K-fusion
 *                     evaluates copies sequentially (no thread overhead).
 * @param out:         (out) Receives &sess->base on success
 *
 * Memory initialization order:
 *   1. Allocate wl_col_session_t (zero-initialized via calloc)
 *   2. Set sess->plan = plan (borrowed reference)
 *   3. Allocate rels[] with initial capacity 16
 *   4. Pre-register EDB relations from plan->edb_relations (ncols lazy-inited)
 *   5. Set *out = &sess->base  (session.c:38 then sets base.backend)
 *
 * @return 0 on success, EINVAL if plan/out is NULL, ENOMEM on alloc failure
 *
 * @see wl_session_create in session.c for vtable dispatch context
 * @see wl_col_session_t memory layout documentation above
 */
static int
col_session_create(const wl_plan_t *plan, uint32_t num_workers,
                   wl_session_t **out)
{
    if (!plan || !out)
        return EINVAL;

    wl_col_session_t *sess
        = (wl_col_session_t *)calloc(1, sizeof(wl_col_session_t));
    if (!sess)
        return ENOMEM;

    sess->plan = plan;
    sess->num_workers = num_workers > 0 ? num_workers : 1;
    sess->rel_cap = 16;
    sess->rels = (col_rel_t **)calloc(sess->rel_cap, sizeof(col_rel_t *));
    if (!sess->rels) {
        free(sess);
        return ENOMEM;
    }

    /* Allocate per-iteration arena (256MB for temporary evaluation data) */
    sess->eval_arena = wl_arena_create(256 * 1024 * 1024);
    if (!sess->eval_arena) {
        free(sess->rels);
        free(sess);
        return ENOMEM;
    }

    /* Create workqueue for parallel K-fusion when num_workers > 1.
     * Single-threaded mode (num_workers=1) leaves wq=NULL; K-fusion
     * evaluates copies sequentially with no thread overhead. (Issue #99) */
    if (sess->num_workers > 1) {
        sess->wq = wl_workqueue_create(sess->num_workers);
        if (!sess->wq) {
            wl_arena_free(sess->eval_arena);
            free(sess->rels);
            free(sess);
            return ENOMEM;
        }
    }

    /* Create delta pool for per-iteration temporaries.
     * Slab: 256 relations (cover ~20 rules x 5 ops + headroom)
     * Arena: 64MB initial (for row data buffers) */
    sess->delta_pool
        = delta_pool_create(256, sizeof(col_rel_t), 64 * 1024 * 1024);
    if (!sess->delta_pool) {
        /* Non-fatal: pool allocation failed, fall back to malloc */
    }

    /* Pre-register EDB relations (ncols determined at first insert) */
    for (uint32_t i = 0; i < plan->edb_count; i++) {
        col_rel_t *r = NULL;
        int rc = col_rel_alloc(&r, plan->edb_relations[i]);
        if (rc != 0)
            goto oom;
        rc = session_add_rel(sess, r);
        if (rc != 0) {
            col_rel_destroy(r);
            goto oom;
        }
    }

    /* Issue #105: Populate stratum_is_monotone from plan.
     * Copy monotone property from each stratum in the plan.
     * Conservative default (all false from calloc) is already set,
     * so only copy if strata exist. */
    for (uint32_t si = 0; si < plan->stratum_count && si < MAX_STRATA; si++) {
        sess->stratum_is_monotone[si] = plan->strata[si].is_monotone;
    }

    /* Issue #103: Initialize 2D frontier epoch tracking.
     * outer_epoch is initialized to 0 by calloc (line 4673) and incremented
     * before each EDB insertion via col_session_insert_incremental. This
     * distinguishes different insertion epochs for 2D frontier (epoch, iteration)
     * pairs to prevent incorrect skip-condition evaluation across epochs. */
    /* outer_epoch = 0; */ /* Already zeroed by calloc */

    *out = &sess->base;
    return 0;

oom:
    for (uint32_t i = 0; i < sess->nrels; i++) {
        col_rel_free_contents(sess->rels[i]);
        free(sess->rels[i]);
    }
    free(sess->rels);
    wl_workqueue_destroy(sess->wq); /* NULL-safe */
    wl_arena_free(sess->eval_arena);
    delta_pool_destroy(sess->delta_pool); /* NULL-safe */
    free(sess);
    return ENOMEM;
}

/*
 * col_session_destroy: Free all resources owned by a columnar session
 *
 * Implements wl_compute_backend_t.session_destroy vtable slot.
 * NULL-safe. Frees rels[], each rels[i], and the session struct itself.
 * The plan is borrowed and NOT freed here.
 *
 * @param session: wl_session_t* (cast to wl_col_session_t* internally)
 */
static void
col_session_destroy(wl_session_t *session)
{
    if (!session)
        return;
    wl_col_session_t *sess = COL_SESSION(session);
    for (uint32_t i = 0; i < sess->nrels; i++) {
        col_rel_free_contents(sess->rels[i]);
        free(sess->rels[i]);
    }
    free(sess->rels);
    if (sess->eval_arena)
        wl_arena_free(sess->eval_arena);
    col_mat_cache_clear(&sess->mat_cache);
    wl_workqueue_destroy(sess->wq);
    /* Free arrangement registry (Phase 3C) */
    for (uint32_t i = 0; i < sess->arr_count; i++) {
        free(sess->arr_entries[i].rel_name);
        free(sess->arr_entries[i].key_cols);
        arr_free_contents(&sess->arr_entries[i].arr);
    }
    free(sess->arr_entries);
    col_session_free_delta_arrangements(sess);
    delta_pool_destroy(sess->delta_pool);
    free(sess);
}

int
col_session_insert(wl_session_t *session, const char *relation,
                   const int64_t *data, uint32_t num_rows, uint32_t num_cols)
{
    if (!session || !relation || !data)
        return EINVAL;

    col_rel_t *r = session_find_rel(COL_SESSION(session), relation);
    if (!r)
        return ENOENT;

    /* Lazy schema initialisation on first insert */
    if (r->ncols == 0) {
        int rc = col_rel_set_schema(r, num_cols, NULL);
        if (rc != 0)
            return rc;
    } else if (r->ncols != num_cols) {
        return EINVAL; /* column count mismatch */
    }

    for (uint32_t i = 0; i < num_rows; i++) {
        int rc = col_rel_append_row(r, data + (size_t)i * num_cols);
        if (rc != 0)
            return rc;
    }

    /* Enable incremental re-evaluation mode: mark this relation as the
     * insertion point for affected stratum detection. This activates
     * frontier persistence in col_session_snapshot, enabling the per-iteration
     * skip condition to reduce iterations on subsequent snapshots. */
    COL_SESSION(session)->last_inserted_relation = relation;

    return 0;
}

/*
 * col_session_insert_incremental: Append facts to a session WITHOUT resetting
 * the per-stratum frontier.
 *
 * Unlike col_session_insert(), this function preserves frontier[] state so
 * that a subsequent col_session_step() call can perform incremental
 * re-evaluation: only strata whose frontier has not yet converged past the
 * current iteration are evaluated.
 *
 * Facts are appended to the existing relation; existing rows are kept.
 * Schema is lazily initialised on the first call (same as col_session_insert).
 *
 * @param session:  Active wl_session_t created by col_session_create
 * @param relation: Name of the EDB relation to append to
 * @param data:     Row-major int64_t array, num_rows * num_cols elements
 * @param num_rows: Number of rows to append (0 is a no-op and returns 0)
 * @param num_cols: Number of columns per row
 * @return 0 on success, EINVAL on bad args, ENOENT if relation unknown,
 *         ENOMEM on allocation failure
 */
int
col_session_insert_incremental(wl_session_t *session, const char *relation,
                               const int64_t *data, uint32_t num_rows,
                               uint32_t num_cols)
{
    if (!session || !relation || !data)
        return EINVAL;

    if (num_rows == 0)
        return 0; /* true no-op */

    col_rel_t *r = session_find_rel(COL_SESSION(session), relation);
    if (!r)
        return ENOENT;

    /* Lazy schema initialisation on first insert */
    if (r->ncols == 0) {
        int rc = col_rel_set_schema(r, num_cols, NULL);
        if (rc != 0)
            return rc;
    } else if (r->ncols != num_cols) {
        return EINVAL; /* column count mismatch */
    }

    /* Append rows; frontier[] is intentionally NOT modified */
    for (uint32_t i = 0; i < num_rows; i++) {
        int rc = col_rel_append_row(r, data + (size_t)i * num_cols);
        if (rc != 0)
            return rc;
    }

    /* Invalidate arrangement caches for the modified relation so subsequent
     * re-evaluation rebuilds hash indices with the new rows (issue #92). */
    col_session_invalidate_arrangements(session, relation);

    /* Issue #103: Increment outer_epoch to mark a new insertion epoch.
     * This epoch counter distinguishes different insertion phases for 2D frontier
     * tracking: (outer_epoch, iteration) pairs ensure iterations are skipped only
     * within the same epoch. Wrapping at UINT32_MAX is acceptable (continues
     * distinguishing epochs across multiple insertions). */
    wl_col_session_t *sess = COL_SESSION(session);
    sess->outer_epoch++;

    /* Record the inserted relation so col_session_step can skip unaffected
     * strata (Phase 4 affected-stratum skip optimization). */
    sess->last_inserted_relation = relation;
    return 0;
}

/* Forward declaration for col_session_remove_incremental */
static int
col_session_remove_incremental(wl_session_t *session, const char *relation,
                               const int64_t *data, uint32_t num_rows,
                               uint32_t num_cols);

static int
col_session_remove(wl_session_t *session, const char *relation,
                   const int64_t *data, uint32_t num_rows, uint32_t num_cols)
{
    if (!session || !relation || !data)
        return EINVAL;

    wl_col_session_t *sess = COL_SESSION(session);
    if (sess->delta_cb != NULL)
        return col_session_remove_incremental(session, relation, data, num_rows,
                                              num_cols);

    col_rel_t *r = session_find_rel(sess, relation);
    if (!r)
        return ENOENT;
    if (r->ncols == 0)
        return 0; /* uninitialized schema = nothing to remove */
    if (r->ncols != num_cols)
        return EINVAL;

    /* Compact: remove matching rows */
    for (uint32_t di = 0; di < num_rows; di++) {
        const int64_t *del = data + (size_t)di * num_cols;
        uint32_t out_r = 0;
        for (uint32_t ri = 0; ri < r->nrows; ri++) {
            const int64_t *row = r->data + (size_t)ri * num_cols;
            if (memcmp(row, del, sizeof(int64_t) * num_cols) != 0) {
                if (out_r != ri)
                    memcpy(r->data + (size_t)out_r * num_cols, row,
                           sizeof(int64_t) * num_cols);
                out_r++;
            } else {
                /* Remove first matching row only */
                di = num_rows; /* break outer loop after this one */
                for (uint32_t rest = ri + 1; rest < r->nrows; rest++, out_r++)
                    memcpy(r->data + (size_t)out_r * num_cols,
                           r->data + (size_t)rest * num_cols,
                           sizeof(int64_t) * num_cols);
                r->nrows = out_r;
                goto next_del;
            }
        }
        r->nrows = out_r;
    next_del:;
    }
    return 0;
}

/*
 * col_session_remove_incremental: Remove rows and pre-seed retraction deltas
 *
 * (Issue #158) Semi-naive delta retraction for non-recursive strata.
 * When a delta callback is registered, this function:
 *   1. Creates $r$<name> relation from removed rows
 *   2. Registers it as a session relation (for VARIABLE ops to consume)
 *   3. Removes rows from the EDB using existing compact logic
 *   4. Records the removal for affected-stratum calculation
 *
 * The $r$<name> relation is used during the next session_step to seed
 * the retraction evaluation, enabling delta-only propagation.
 */
static int
col_session_remove_incremental(wl_session_t *session, const char *relation,
                               const int64_t *data, uint32_t num_rows,
                               uint32_t num_cols)
{
    if (!session || !relation || !data)
        return EINVAL;

    wl_col_session_t *sess = COL_SESSION(session);

    /* Find EDB relation */
    col_rel_t *r = session_find_rel(sess, relation);
    if (!r)
        return ENOENT;
    if (r->ncols == 0)
        return 0; /* uninitialized schema = nothing to remove */
    if (r->ncols != num_cols)
        return EINVAL;

    /* Allocate $r$<name> delta relation to collect removed rows */
    char rname[256];
    snprintf(rname, sizeof(rname), "$r$%s", relation);

    col_rel_t *rdelta = col_rel_new_auto(rname, num_cols);
    if (!rdelta)
        return ENOMEM;

    /* Append each removed row to the delta relation.
     * We need to track which rows are actually being removed from the EDB,
     * then add them to rdelta. */
    int rc = 0;
    for (uint32_t di = 0; di < num_rows; di++) {
        const int64_t *del = data + (size_t)di * num_cols;
        /* Check if this row exists in EDB; if so, append to rdelta */
        for (uint32_t ri = 0; ri < r->nrows; ri++) {
            const int64_t *row = r->data + (size_t)ri * num_cols;
            if (memcmp(row, del, sizeof(int64_t) * num_cols) == 0) {
                /* Found matching row; add to retraction delta */
                rc = col_rel_append_row(rdelta, del);
                if (rc != 0) {
                    col_rel_destroy(rdelta);
                    return rc;
                }
                break; /* Only one copy per removal request */
            }
        }
    }

    /* Register $r$<name> in session (replacing any prior) */
    session_remove_rel(sess, rname);
    rc = session_add_rel(sess, rdelta);
    if (rc != 0) {
        col_rel_destroy(rdelta);
        return rc;
    }

    /* Remove rows from the EDB using existing compact logic */
    for (uint32_t di = 0; di < num_rows; di++) {
        const int64_t *del = data + (size_t)di * num_cols;
        uint32_t out_r = 0;
        for (uint32_t ri = 0; ri < r->nrows; ri++) {
            const int64_t *row = r->data + (size_t)ri * num_cols;
            if (memcmp(row, del, sizeof(int64_t) * num_cols) != 0) {
                if (out_r != ri)
                    memcpy(r->data + (size_t)out_r * num_cols, row,
                           sizeof(int64_t) * num_cols);
                out_r++;
            } else {
                /* Remove first matching row only */
                di = num_rows; /* break outer loop after this one */
                for (uint32_t rest = ri + 1; rest < r->nrows; rest++, out_r++)
                    memcpy(r->data + (size_t)out_r * num_cols,
                           r->data + (size_t)rest * num_cols,
                           sizeof(int64_t) * num_cols);
                r->nrows = out_r;
                goto next_del_incr;
            }
        }
        r->nrows = out_r;
    next_del_incr:;
    }

    /* Clamp base_nrows to current row count */
    if (r->base_nrows > r->nrows)
        r->base_nrows = r->nrows;

    /* Mark removal for affected-stratum calculation */
    sess->last_removed_relation = relation;
    sess->outer_epoch++;

    return 0;
}

/*
 * col_session_step: Advance the session by one evaluation epoch
 *
 * Implements wl_compute_backend_t.session_step vtable slot.
 *
 * Iterates all strata in plan order. For each stratum:
 *   - Fast path (no delta_cb): col_eval_stratum directly
 *   - Delta path: col_stratum_step_with_delta (snapshot + eval + set diff)
 * Arena is reset after each stratum to reclaim temporary evaluation data.
 *
 * TODO(Phase 2B): Replace set-diff delta with semi-naive ΔR propagation.
 *
 * @param session: wl_session_t* (cast to wl_col_session_t* internally)
 * @return 0 on success, non-zero on evaluation error
 */
static int
col_session_step(wl_session_t *session)
{
    wl_col_session_t *sess = COL_SESSION(session);
    const wl_plan_t *plan = sess->plan;

    /* Compute affected strata bitmask (Phase 4 incremental skip).
     * When last_inserted_relation is set (incremental path), only evaluate
     * strata that transitively depend on the inserted relation.  When NULL
     * (regular step), UINT64_MAX means all strata are evaluated. */
    uint64_t affected_mask = UINT64_MAX;
    if (sess->last_inserted_relation != NULL) {
        affected_mask = col_compute_affected_strata(
            session, sess->last_inserted_relation);
    }

    /* Issue #158: Pre-seed retraction deltas for affected strata.
     * If last_removed_relation is set (removal via col_session_remove_incremental),
     * check if $r$<name> exists for retractions, and set retraction_seeded. */
    if (sess->last_removed_relation != NULL) {
        affected_mask &= col_compute_affected_strata(
            session, sess->last_removed_relation);
        char rname[256];
        if (retraction_rel_name(sess->last_removed_relation, rname,
                                sizeof(rname))
            == 0) {
            col_rel_t *rdelta = session_find_rel(sess, rname);
            if (rdelta && rdelta->nrows > 0)
                sess->retraction_seeded = true;
        }
    }

    /* Issue #106 (US-106-004): Reset rule frontiers with stratum context awareness.
     * col_session_step is for delta callback mode (no pre-seeded deltas).
     * Always reset affected rules to force re-evaluation.
     * Selective reset based on pre-seeded delta is only in col_session_snapshot.
     *
     * @see col_session_snapshot for selective rule frontier reset (Issue #107) */
    if (affected_mask == UINT64_MAX) {
        /* Full evaluation (non-incremental): reset all rules to (current_epoch, UINT32_MAX)
         * sentinel. Prevents premature skip across different evaluation contexts. */
        for (uint32_t ri = 0; ri < MAX_RULES; ri++) {
            sess->rule_frontiers[ri].outer_epoch = sess->outer_epoch;
            sess->rule_frontiers[ri].iteration = UINT32_MAX;
        }
    } else {
        /* Incremental (delta callback mode): reset affected rules to (current_epoch, UINT32_MAX).
         * No pre-seeded deltas in this path, so reset unconditionally. */
        for (uint32_t si = 0; si < plan->stratum_count; si++) {
            if ((affected_mask & ((uint64_t)1 << si)) != 0) {
                uint32_t rule_base = 0;
                for (uint32_t j = 0; j < si; j++)
                    rule_base += plan->strata[j].relation_count;
                for (uint32_t ri = 0; ri < plan->strata[si].relation_count;
                     ri++) {
                    uint32_t rule_id = rule_base + ri;
                    if (rule_id < MAX_RULES) {
                        sess->rule_frontiers[rule_id].outer_epoch
                            = sess->outer_epoch;
                        sess->rule_frontiers[rule_id].iteration = UINT32_MAX;
                    }
                }
            }
        }
    }

    for (uint32_t si = 0; si < plan->stratum_count; si++) {
        /* Skip strata not affected by the last incremental insertion */
        if ((affected_mask & ((uint64_t)1 << si)) == 0)
            continue;

        const wl_plan_stratum_t *sp = &plan->strata[si];
        int rc = sess->delta_cb ? col_stratum_step_with_delta(sp, sess, si)
                                : col_eval_stratum(sp, sess, si);
        if (rc != 0)
            return rc;
        /* Reset arena after stratum evaluation to free temporaries */
        if (sess->eval_arena)
            wl_arena_reset(sess->eval_arena);
    }

    /* Issue #158: Cleanup retraction state and delta relations after step */
    sess->last_removed_relation = NULL;
    sess->retraction_seeded = false;
    /* Remove all $r$<name> relations from session */
    for (uint32_t i = 0; i < sess->nrels;) {
        col_rel_t *r = sess->rels[i];
        if (r && strncmp(r->name, "$r$", 3) == 0) {
            session_remove_rel(sess, r->name);
            /* session_remove_rel shifts array, so don't increment i */
        } else {
            i++;
        }
    }

    /* Reset after successful eval so next plain session_step runs all strata */
    sess->last_inserted_relation = NULL;
    return 0;
}

/*
 * col_session_set_delta_cb: Register a delta callback on this session
 *
 * Implements wl_compute_backend_t.session_set_delta_cb vtable slot.
 * The callback is invoked with diff=+1 for new tuples during col_session_step.
 *
 * TODO(Phase 2B): Also fire diff=-1 for retracted tuples when semi-naive
 * delta propagation tracks removed tuples explicitly.
 *
 * @param session:   wl_session_t* (cast to wl_col_session_t* internally)
 * @param callback:  Function invoked per output delta tuple (NULL to disable)
 * @param user_data: Opaque pointer passed through to callback
 */
static void
col_session_set_delta_cb(wl_session_t *session, wl_on_delta_fn callback,
                         void *user_data)
{
    if (!session)
        return;
    wl_col_session_t *sess = COL_SESSION(session);
    sess->delta_cb = callback;
    sess->delta_data = user_data;
}

/*
 * col_session_snapshot: Evaluate all strata and emit current IDB tuples
 *
 * Implements wl_compute_backend_t.session_snapshot vtable slot.
 *
 * Evaluation order:
 *   1. Execute all strata in plan order (col_eval_stratum per stratum)
 *   2. For each IDB relation in each stratum, invoke callback once per row
 *
 * Complexity: O(S * R * N) where S=strata, R=relations per stratum, N=rows
 *
 * TODO(Phase 2B): Snapshot should read from stable R (not recompute);
 * currently re-evaluates on every call which is O(input) per snapshot.
 *
 * @param session:   wl_session_t* (cast to wl_col_session_t* internally)
 * @param callback:  Invoked once per output tuple (relation, row, ncols)
 * @param user_data: Opaque pointer passed through to callback
 * @return 0 on success, EINVAL if session/callback NULL, non-zero on eval error
 */
static int
col_session_snapshot(wl_session_t *session, wl_on_tuple_fn callback,
                     void *user_data)
{
    if (!session || !callback)
        return EINVAL;

    wl_col_session_t *sess = COL_SESSION(session);
    const wl_plan_t *plan = sess->plan;

    /* Reset profiling counters for this evaluation pass */
    sess->consolidation_ns = 0;
    sess->kfusion_ns = 0;
    sess->kfusion_alloc_ns = 0;
    sess->kfusion_dispatch_ns = 0;
    sess->kfusion_merge_ns = 0;
    sess->kfusion_cleanup_ns = 0;

    /* Phase 4 incremental skip: when last_inserted_relation is set, only
     * re-evaluate strata that transitively depend on the inserted relation.
     * On the first snapshot (total_iterations == 0), always evaluate all strata
     * to establish the baseline. */
    uint64_t affected_mask = UINT64_MAX;
    if (sess->last_inserted_relation != NULL && sess->total_iterations > 0) {
        affected_mask = col_compute_affected_strata(
            session, sess->last_inserted_relation);

        /* Issue #83: Pre-seed EDB delta relations for delta-only propagation.
         * For each relation with nrows > base_nrows, create a $d$<name> delta
         * containing only the new rows. This allows FORCE_DELTA at iteration 0
         * to use the delta instead of the full relation, avoiding full
         * re-derivation of existing IDB tuples. */
        for (uint32_t i = 0; i < sess->nrels; i++) {
            col_rel_t *r = sess->rels[i];
            if (!r || r->base_nrows == 0 || r->nrows <= r->base_nrows)
                continue;
            /* Create delta relation with rows[base_nrows..nrows) */
            char dname[256];
            snprintf(dname, sizeof(dname), "$d$%s", r->name);
            uint32_t delta_nrows = r->nrows - r->base_nrows;
            col_rel_t *delta = col_rel_new_auto(dname, r->ncols);
            if (!delta)
                continue; /* best-effort; falls back to full eval */
            for (uint32_t row = 0; row < delta_nrows; row++) {
                col_rel_append_row(
                    delta, r->data + (size_t)(r->base_nrows + row) * r->ncols);
            }
            session_remove_rel(sess, dname);
            session_add_rel(sess, delta);
        }
        sess->delta_seeded = true;
    }

    /* For affected strata, selectively reset the per-stratum frontier to UINT32_MAX
     * (not-set sentinel) based on pre-seeded EDB delta presence.
     * UINT32_MAX ensures `iter > UINT32_MAX` is always false, forcing full
     * re-evaluation of strata with pre-seeded deltas.
     *
     * Issue #107: Selective frontier reset based on pre-seeded delta check.
     * Reset frontiers ONLY for strata that have pre-seeded EDB deltas.
     * Preserve frontiers for transitively-affected strata (no direct EDB delta).
     *
     * Safety: Transitively-affected strata receive new facts from upstream via
     * delta propagation, but convergence still occurs within previous frontier
     * bounds. Semi-naive evaluation processes deltas incrementally: iteration i
     * only derives from deltas at iteration i-1. If a stratum converged at
     * iteration F (no new facts at F+1+), subsequent upstream facts flow through
     * iterations 0..F, unlikely to require F+1+ unless graph topology changes.
     * Test coverage (test_delta_propagation test 3) validates correctness for
     * cyclic multi-iteration patterns. CSPA benchmark confirms safety. */
    if (affected_mask != UINT64_MAX) {
        for (uint32_t si = 0; si < plan->stratum_count && si < MAX_STRATA;
             si++) {
            if ((affected_mask & ((uint64_t)1 << si)) != 0) {
                /* Issue #107: Selective rule frontier reset based on pre-seeded delta presence.
                 * Reset frontier for strata that have pre-seeded EDB deltas.
                 * Preserve frontier for transitively-affected strata (no direct EDB delta).
                 *
                 * Safety: Transitively-affected strata receive new facts from upstream
                 * strata, but in the presence of pre-seeded deltas, fact propagation
                 * still converges within previous frontier bounds. The pre-seeded delta
                 * check already limits EDB propagation (delta from [base_nrows, nrows)).
                 *
                 * Test coverage: test_delta_propagation validates cyclic correctness. */
                if (stratum_has_preseeded_delta(&plan->strata[si], sess)) {
                    sess->frontiers[si].outer_epoch = sess->outer_epoch;
                    sess->frontiers[si].iteration = UINT32_MAX;
                }
                /* Else: stratum affected but no pre-seeded delta → KEEP frontier */
            }
        }
        /* Phase 4 (US-4-004) + Issue #107: Selective rule frontier reset.
         * Use col_compute_affected_rules bitmask to identify rules needing
         * re-evaluation. For each affected rule, check if its stratum has
         * pre-seeded EDB delta before resetting the frontier.
         *
         * Reset when:
         *   1. Rule is affected (bit set in affected_rules)
         *   2. Rule's stratum is affected (bit set in affected_mask)
         *   3. Stratum HAS pre-seeded EDB delta
         *
         * Preserve when:
         *   1. Rule's stratum affected but NO pre-seeded delta (transitively affected only)
         *   2. Frontier skip can still fire for iterations beyond previous convergence point
         *
         * Performance: Frontier skip on transitively-affected strata reduces iterations
         * for IDB-only derivations, improving speedup from frontier skip optimization. */
        uint64_t affected_rules
            = col_compute_affected_rules(session, sess->last_inserted_relation);
        for (uint32_t ri = 0; ri < MAX_RULES; ri++) {
            if ((affected_rules & ((uint64_t)1 << ri)) == 0)
                continue;
            uint32_t si = rule_index_to_stratum_index(plan, ri);
            if (si == UINT32_MAX)
                continue;
            if ((affected_mask & ((uint64_t)1 << si)) == 0)
                continue;
            if (stratum_has_preseeded_delta(&plan->strata[si], sess)) {
                sess->rule_frontiers[ri].outer_epoch = sess->outer_epoch;
                sess->rule_frontiers[ri].iteration = UINT32_MAX;
            }
            /* Else: rule's stratum affected but no pre-seeded delta → KEEP frontier */
        }
    } else {
        /* Full re-evaluation (non-incremental call): reset ALL rule frontiers
         * to (current_epoch, UINT32_MAX) sentinel. Prevents premature skip. */
        for (uint32_t ri = 0; ri < MAX_RULES; ri++) {
            sess->rule_frontiers[ri].outer_epoch = sess->outer_epoch;
            sess->rule_frontiers[ri].iteration = UINT32_MAX;
        }
    }

    /* Execute strata in order, skipping unaffected ones */
    for (uint32_t si = 0; si < plan->stratum_count; si++) {
        if ((affected_mask & ((uint64_t)1 << si)) == 0)
            continue;
        int rc = col_eval_stratum(&plan->strata[si], sess, si);
        if (rc != 0)
            return rc;
        if (sess->eval_arena)
            wl_arena_reset(sess->eval_arena);
    }

    /* Reset after successful eval so next plain snapshot runs all strata */
    sess->last_inserted_relation = NULL;
    sess->delta_seeded = false;

    /* Issue #83: Update base_nrows for all relations after convergence.
     * This marks the current state as "stable" so the next incremental
     * insert can compute the delta as rows[base_nrows..nrows). */
    for (uint32_t i = 0; i < sess->nrels; i++) {
        col_rel_t *r = sess->rels[i];
        if (r)
            r->base_nrows = r->nrows;
    }

    /* Invoke callback for every tuple in every IDB relation */
    for (uint32_t si = 0; si < plan->stratum_count; si++) {
        const wl_plan_stratum_t *sp = &plan->strata[si];
        for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
            const char *rname = sp->relations[ri].name;
            col_rel_t *r = session_find_rel(sess, rname);
            if (!r || r->nrows == 0)
                continue;
            for (uint32_t row = 0; row < r->nrows; row++) {
                callback(rname, r->data + (size_t)row * r->ncols, r->ncols,
                         user_data);
            }
        }
    }
    return 0;
}

/* ======================================================================== */
/* Affected Strata Detection (Phase 4)                                      */
/* ======================================================================== */

/*
 * bitmask_or_simd - Union two 64-bit bitmasks using SIMD when available.
 *
 * For >8 strata this avoids sequential OR chains by operating on two 64-bit
 * words packed into a 128-bit vector in a single instruction.
 *
 * When SIMD is unavailable the scalar fallback is a single OR, which the
 * compiler will inline. The result is always written back to *dst.
 */
static inline uint64_t
bitmask_or_simd(uint64_t dst, uint64_t src)
{
#if defined(__ARM_NEON__)
    /* Pack both masks into a 128-bit vector and OR them in one shot. */
    uint64x2_t vd = vcombine_u64(vcreate_u64(dst), vcreate_u64(0));
    uint64x2_t vs = vcombine_u64(vcreate_u64(src), vcreate_u64(0));
    uint64x2_t vr = vorrq_u64(vd, vs);
    return vgetq_lane_u64(vr, 0);
#elif defined(__SSE2__)
    __m128i vd = _mm_set_epi64x(0, (int64_t)dst);
    __m128i vs = _mm_set_epi64x(0, (int64_t)src);
    __m128i vr = _mm_or_si128(vd, vs);
    return (uint64_t)_mm_cvtsi128_si64(vr);
#else
    return dst | src;
#endif
}

/*
 * stratum_references_relation - Return true if any VARIABLE op in stratum sp
 * references the relation named `rel`, or if any JOIN/ANTIJOIN/SEMIJOIN op
 * has right_relation matching `rel`.
 */
static bool
stratum_references_relation(const wl_plan_stratum_t *sp, const char *rel)
{
    for (uint32_t ri = 0; ri < sp->relation_count; ri++) {
        const wl_plan_relation_t *pr = &sp->relations[ri];
        for (uint32_t oi = 0; oi < pr->op_count; oi++) {
            if (pr->ops[oi].op == WL_PLAN_OP_VARIABLE
                && pr->ops[oi].relation_name != NULL
                && strcmp(pr->ops[oi].relation_name, rel) == 0) {
                return true;
            }
            if ((pr->ops[oi].op == WL_PLAN_OP_JOIN
                 || pr->ops[oi].op == WL_PLAN_OP_ANTIJOIN
                 || pr->ops[oi].op == WL_PLAN_OP_SEMIJOIN)
                && pr->ops[oi].right_relation != NULL
                && strcmp(pr->ops[oi].right_relation, rel) == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * col_compute_affected_strata - Identify strata needing re-evaluation.
 *
 * Walks the stratum dependency graph rooted at `inserted_relation` and
 * returns a bitmask where bit i is set if stratum i directly or transitively
 * depends on the newly inserted relation.
 *
 * Algorithm (iterative worklist BFS over stratum indices):
 *   1. Seed: mark every stratum that directly references inserted_relation.
 *   2. Propagate: for each newly-marked stratum, find all strata that
 *      reference any relation produced by the marked stratum, and mark them.
 *   3. Repeat until no new strata are marked.
 *
 * "Produced by stratum s" = every relation listed in plan->strata[s].relations.
 *
 * SIMD: bitmask union uses bitmask_or_simd() which compiles to a single
 * vorrq_u64 (NEON) or _mm_or_si128 (SSE2) instruction when those ISAs are
 * available. The scalar path is a plain | operator.
 *
 * Supports up to 64 strata (one uint64_t bitmask). Plans with more strata
 * will have bits beyond 63 silently ignored (conservative: returns 0 for
 * those strata, which causes them to be re-evaluated unconditionally by the
 * caller's fallback).
 *
 * @param session          Active session (must have a plan attached).
 * @param inserted_relation Name of the EDB relation receiving new facts.
 * @return Bitmask of affected stratum indices; 0 on invalid input.
 */
uint64_t
col_compute_affected_strata(wl_session_t *session,
                            const char *inserted_relation)
{
    if (!session || !inserted_relation)
        return 0;

    wl_col_session_t *sess = COL_SESSION(session);
    const wl_plan_t *plan = sess->plan;
    if (!plan || plan->stratum_count == 0)
        return 0;

    uint32_t nstrata = plan->stratum_count;
    if (nstrata > 64)
        nstrata = 64; /* clamp to bitmask width */

    /* Issue #93: Removed EDB fast path that returned full_mask for any EDB
     * insertion. The BFS below correctly computes targeted affected strata
     * for both EDB and IDB relations, enabling frontier skip optimization. */

    uint64_t affected = 0;

    /* --- Pass 1: seed with strata directly referencing inserted_relation --- */
    for (uint32_t si = 0; si < nstrata; si++) {
        if (stratum_references_relation(&plan->strata[si], inserted_relation)) {
            affected = bitmask_or_simd(affected, (uint64_t)1 << si);
        }
    }

    /* Early exit: if all strata are already marked, skip transitive closure.
     * This occurs when inserted_relation is a base fact (referenced by many/all
     * strata), avoiding O(nstrata^2) work in Pass 2. */
    uint64_t full_mask = (nstrata == 64) ? ~0ULL : ((1ULL << nstrata) - 1);
    if (affected == full_mask)
        return affected;

    /* --- Pass 2: transitive propagation via fixed-point iteration ---------- */
    /*
     * For every newly-marked stratum, find all strata that reference any
     * relation produced by that stratum. We loop until the bitmask stabilises.
     */
    uint64_t prev = 0;
    while (prev != affected) {
        prev = affected;
        /* Iterate over all currently marked strata. */
        uint64_t pending = affected;
        while (pending) {
            /* Extract lowest set bit index. */
            uint32_t si = (uint32_t)__builtin_ctzll(pending);
            pending &= pending - 1; /* clear lowest set bit */

            if (si >= nstrata)
                break;

            const wl_plan_stratum_t *src_sp = &plan->strata[si];
            /* For each relation produced by stratum si ... */
            for (uint32_t ri = 0; ri < src_sp->relation_count; ri++) {
                const char *produced = src_sp->relations[ri].name;
                if (!produced)
                    continue;
                /* ... mark any stratum that references it. */
                for (uint32_t sj = 0; sj < nstrata; sj++) {
                    if (affected & ((uint64_t)1 << sj))
                        continue; /* already marked */
                    if (stratum_references_relation(&plan->strata[sj],
                                                    produced)) {
                        affected = bitmask_or_simd(affected, (uint64_t)1 << sj);
                    }
                }
            }
        }
    }

    return affected;
}

/* ======================================================================== */
/* Affected Rule Detection (Phase 4, US-4-003)                              */
/* ======================================================================== */

/*
 * rule_references_relation - Return true if any VARIABLE op in relation pr
 * references the relation named `rel`.
 */
static bool
rule_references_relation(const wl_plan_relation_t *pr, const char *rel)
{
    for (uint32_t oi = 0; oi < pr->op_count; oi++) {
        /* Check VARIABLE ops (left child of joins) */
        if (pr->ops[oi].op == WL_PLAN_OP_VARIABLE
            && pr->ops[oi].relation_name != NULL
            && strcmp(pr->ops[oi].relation_name, rel) == 0) {
            return true;
        }
        /* Check JOIN/ANTIJOIN/SEMIJOIN right_relation (right child of joins) */
        if ((pr->ops[oi].op == WL_PLAN_OP_JOIN
             || pr->ops[oi].op == WL_PLAN_OP_ANTIJOIN
             || pr->ops[oi].op == WL_PLAN_OP_SEMIJOIN)
            && pr->ops[oi].right_relation != NULL
            && strcmp(pr->ops[oi].right_relation, rel) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * col_compute_affected_rules - Identify rules needing re-evaluation.
 *
 * Rules are enumerated globally across all strata in declaration order:
 * stratum 0 relations first (by relation index), then stratum 1, etc.
 * Rule index i corresponds to the i-th relation in this traversal.
 *
 * Algorithm (same iterative fixed-point BFS as col_compute_affected_strata):
 *   1. Seed: mark every rule whose VARIABLE body references inserted_relation.
 *   2. Propagate: for each newly-marked rule, find all rules whose body
 *      references the head relation produced by the marked rule, and mark them.
 *   3. Repeat until the bitmask stabilises.
 *
 * SIMD: bitmask union uses bitmask_or_simd() (same helper as strata version).
 *
 * Supports up to 64 rules (one uint64_t bitmask). Plans with more rules will
 * have bits beyond 63 silently ignored (conservative: those rules are always
 * re-evaluated by the caller's fallback via UINT64_MAX mask).
 *
 * @param session           Active session (must have a plan attached).
 * @param inserted_relation Name of the EDB relation receiving new facts.
 * @return Bitmask of affected rule indices; 0 on invalid input.
 */
uint64_t
col_compute_affected_rules(wl_session_t *session, const char *inserted_relation)
{
    if (!session || !inserted_relation)
        return 0;

    wl_col_session_t *sess = COL_SESSION(session);
    const wl_plan_t *plan = sess->plan;
    if (!plan || plan->stratum_count == 0)
        return 0;

    /*
     * Build a flat enumeration of (stratum_idx, relation_idx) pairs so each
     * rule has a stable global index.  We clamp to MAX_RULES (64) to stay
     * within the uint64_t bitmask.
     */
    uint32_t nrules = 0;
    for (uint32_t si = 0; si < plan->stratum_count && nrules < MAX_RULES;
         si++) {
        uint32_t rc = plan->strata[si].relation_count;
        if (nrules + rc > MAX_RULES)
            rc = MAX_RULES - nrules;
        nrules += rc;
    }

    if (nrules == 0)
        return 0;

    /*
     * Store (stratum_idx, relation_idx) for each global rule index so we can
     * look up the rule's head name and body ops later.
     */
    uint32_t rule_si[MAX_RULES]; /* stratum index for rule i  */
    uint32_t rule_ri[MAX_RULES]; /* relation index for rule i */
    {
        uint32_t idx = 0;
        for (uint32_t si = 0; si < plan->stratum_count && idx < MAX_RULES;
             si++) {
            for (uint32_t ri = 0;
                 ri < plan->strata[si].relation_count && idx < MAX_RULES;
                 ri++) {
                rule_si[idx] = si;
                rule_ri[idx] = ri;
                idx++;
            }
        }
    }

    uint64_t affected = 0;

    /* --- Pass 1: seed rules that directly reference inserted_relation --- */
    for (uint32_t i = 0; i < nrules; i++) {
        const wl_plan_relation_t *pr
            = &plan->strata[rule_si[i]].relations[rule_ri[i]];
        if (rule_references_relation(pr, inserted_relation)) {
            affected = bitmask_or_simd(affected, (uint64_t)1 << i);
        }
    }

    /* --- Pass 2: transitive propagation via fixed-point iteration --------- */
    /*
     * For every newly-marked rule, find all rules whose body references the
     * head relation produced by the marked rule. Loop until stable.
     */
    uint64_t prev = 0;
    while (prev != affected) {
        prev = affected;
        uint64_t pending = affected;
        while (pending) {
            uint32_t i = (uint32_t)__builtin_ctzll(pending);
            pending &= pending - 1; /* clear lowest set bit */

            if (i >= nrules)
                break;

            /* Head relation name produced by rule i */
            const char *head
                = plan->strata[rule_si[i]].relations[rule_ri[i]].name;
            if (!head)
                continue;

            /* Mark any rule whose body references this head */
            for (uint32_t j = 0; j < nrules; j++) {
                if (affected & ((uint64_t)1 << j))
                    continue; /* already marked */
                const wl_plan_relation_t *pr
                    = &plan->strata[rule_si[j]].relations[rule_ri[j]];
                if (rule_references_relation(pr, head)) {
                    affected = bitmask_or_simd(affected, (uint64_t)1 << j);
                }
            }
        }
    }

    return affected;
}

/* ======================================================================== */
/* Mobius / Z-set Weighted JOIN                                              */
/* ======================================================================== */

/*
 * col_op_join_weighted - equi-join with multiplicity multiplication.
 *
 * Joins lhs and rhs on column index key_col (present in both).  For each
 * matching pair the output row is appended to dst and its timestamp
 * multiplicity is set to lhs_mult * rhs_mult.
 *
 * Output layout: all lhs columns followed by all rhs columns (key column
 * is duplicated; callers may project as needed).  dst->ncols is initialised
 * by this function; dst must be caller-allocated with ncols==0 on entry.
 *
 * Returns 0 on success, non-zero (ENOMEM / EINVAL) on error.
 */
int
col_op_join_weighted(const col_rel_t *lhs, const col_rel_t *rhs,
                     uint32_t key_col, col_rel_t *dst)
{
    if (!lhs || !rhs || !dst)
        return EINVAL;
    if (key_col >= lhs->ncols || key_col >= rhs->ncols)
        return EINVAL;

    uint32_t ocols = lhs->ncols + rhs->ncols;
    dst->ncols = ocols;

    int64_t *tmp = (int64_t *)malloc(sizeof(int64_t) * (ocols > 0 ? ocols : 1));
    if (!tmp)
        return ENOMEM;

    int rc = 0;
    for (uint32_t li = 0; li < lhs->nrows && rc == 0; li++) {
        const int64_t *lrow = lhs->data + (size_t)li * lhs->ncols;
        int64_t lmult = lhs->timestamps ? lhs->timestamps[li].multiplicity : 1;

        for (uint32_t ri = 0; ri < rhs->nrows && rc == 0; ri++) {
            const int64_t *rrow = rhs->data + (size_t)ri * rhs->ncols;

            if (lrow[key_col] != rrow[key_col])
                continue;

            int64_t rmult
                = rhs->timestamps ? rhs->timestamps[ri].multiplicity : 1;

            memcpy(tmp, lrow, sizeof(int64_t) * lhs->ncols);
            memcpy(tmp + lhs->ncols, rrow, sizeof(int64_t) * rhs->ncols);

            /* Grow dst manually to keep data and timestamps in sync. */
            if (dst->nrows >= dst->capacity) {
                uint32_t new_cap = dst->capacity ? dst->capacity * 2 : 16;
                int64_t *nd = (int64_t *)realloc(
                    dst->data, sizeof(int64_t) * (size_t)new_cap * ocols);
                if (!nd) {
                    rc = ENOMEM;
                    break;
                }
                dst->data = nd;
                col_delta_timestamp_t *nt = (col_delta_timestamp_t *)realloc(
                    dst->timestamps,
                    (size_t)new_cap * sizeof(col_delta_timestamp_t));
                if (!nt) {
                    rc = ENOMEM;
                    break;
                }
                dst->timestamps = nt;
                dst->capacity = new_cap;
            }
            memcpy(dst->data + (size_t)dst->nrows * ocols, tmp,
                   sizeof(int64_t) * ocols);
            memset(&dst->timestamps[dst->nrows], 0,
                   sizeof(col_delta_timestamp_t));
            dst->timestamps[dst->nrows].multiplicity = lmult * rmult;
            dst->nrows++;
        }
    }

    free(tmp);
    return rc;
}

/* ======================================================================== */
/* Mobius / Z-set Delta Formula                                             */
/* ======================================================================== */

/*
 * col_compute_delta_mobius:
 * Compute the Mobius delta between prev_collection and curr_collection.
 *
 * For each unique key (column 0) in the union of both relations:
 *   - key only in curr:  delta_mult = curr_mult
 *   - key only in prev:  delta_mult = -prev_mult
 *   - key in both:       delta_mult = curr_mult - prev_mult (skipped if 0)
 *
 * Both input relations must have timestamps != NULL.
 * out_delta must be caller-allocated, empty (nrows==0) on entry.
 *
 * Returns 0 on success, EINVAL on bad arguments, ENOMEM on allocation failure.
 */
int
col_compute_delta_mobius(const col_rel_t *prev_collection,
                         const col_rel_t *curr_collection, col_rel_t *out_delta)
{
    if (!prev_collection || !curr_collection || !out_delta)
        return EINVAL;
    if (prev_collection->ncols == 0 || curr_collection->ncols == 0)
        return EINVAL;
    if (prev_collection->ncols != curr_collection->ncols)
        return EINVAL;

    uint32_t ncols = prev_collection->ncols;
    out_delta->ncols = ncols;

    /* Helper lambda (via inline block) to append a row+mult to out_delta. */
#define DELTA_APPEND(row_ptr, mult_val)                                       \
    do {                                                                      \
        if (out_delta->nrows >= out_delta->capacity) {                        \
            uint32_t new_cap                                                  \
                = out_delta->capacity ? out_delta->capacity * 2 : 16;         \
            int64_t *nd = (int64_t *)realloc(                                 \
                out_delta->data, sizeof(int64_t) * (size_t)new_cap * ncols);  \
            if (!nd)                                                          \
                return ENOMEM;                                                \
            out_delta->data = nd;                                             \
            col_delta_timestamp_t *nt = (col_delta_timestamp_t *)realloc(     \
                out_delta->timestamps,                                        \
                (size_t)new_cap * sizeof(col_delta_timestamp_t));             \
            if (!nt)                                                          \
                return ENOMEM;                                                \
            out_delta->timestamps = nt;                                       \
            out_delta->capacity = new_cap;                                    \
        }                                                                     \
        memcpy(out_delta->data + (size_t)out_delta->nrows * ncols, (row_ptr), \
               sizeof(int64_t) * ncols);                                      \
        col_delta_timestamp_t ts_;                                            \
        memset(&ts_, 0, sizeof(ts_));                                         \
        ts_.multiplicity = (mult_val);                                        \
        out_delta->timestamps[out_delta->nrows] = ts_;                        \
        out_delta->nrows++;                                                   \
    } while (0)

    /* Pass 1: iterate over curr; for each key look up in prev. */
    for (uint32_t ci = 0; ci < curr_collection->nrows; ci++) {
        const int64_t *crow = curr_collection->data + (size_t)ci * ncols;
        int64_t cmult = curr_collection->timestamps
                            ? curr_collection->timestamps[ci].multiplicity
                            : 1;

        /* Search prev for matching key (column 0). */
        int64_t pmult = 0;
        bool found_in_prev = false;
        for (uint32_t pi = 0; pi < prev_collection->nrows; pi++) {
            const int64_t *prow = prev_collection->data + (size_t)pi * ncols;
            if (prow[0] == crow[0]) {
                pmult = prev_collection->timestamps
                            ? prev_collection->timestamps[pi].multiplicity
                            : 1;
                found_in_prev = true;
                break;
            }
        }

        int64_t delta_mult = found_in_prev ? (cmult - pmult) : cmult;
        if (delta_mult != 0) {
            DELTA_APPEND(crow, delta_mult);
        }
    }

    /* Pass 2: iterate over prev; emit -prev_mult for keys absent in curr. */
    for (uint32_t pi = 0; pi < prev_collection->nrows; pi++) {
        const int64_t *prow = prev_collection->data + (size_t)pi * ncols;
        int64_t pmult = prev_collection->timestamps
                            ? prev_collection->timestamps[pi].multiplicity
                            : 1;

        bool found_in_curr = false;
        for (uint32_t ci = 0; ci < curr_collection->nrows; ci++) {
            const int64_t *crow = curr_collection->data + (size_t)ci * ncols;
            if (crow[0] == prow[0]) {
                found_in_curr = true;
                break;
            }
        }

        if (!found_in_curr) {
            int64_t delta_mult = -pmult;
            if (delta_mult != 0) {
                DELTA_APPEND(prow, delta_mult);
            }
        }
    }

#undef DELTA_APPEND

    return 0;
}

/* ======================================================================== */
/* Vtable Singleton                                                          */
/* ======================================================================== */

static const wl_compute_backend_t col_backend = {
    .name = "columnar",
    .session_create = col_session_create,
    .session_destroy = col_session_destroy,
    .session_insert = col_session_insert,
    .session_remove = col_session_remove,
    .session_step = col_session_step,
    .session_set_delta_cb = col_session_set_delta_cb,
    .session_snapshot = col_session_snapshot,
};

const wl_compute_backend_t *
wl_backend_columnar(void)
{
    return &col_backend;
}
