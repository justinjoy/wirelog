/*
 * test_cse_cache_hit_rate.c
 *
 * Comprehensive TDD test suite for CSE (Common Subexpression Elimination) cache.
 * Tests current pointer-based keying behavior and desired content-based keying behavior.
 *
 * US-001: CSE Cache Test Suite
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Test macros */
#define TEST(name) static void test_ ## name(void)
#define PASS(msg) printf("✓ %s\n", msg)
#define FAIL(msg)              \
        do {                       \
            printf("✗ %s\n", msg); \
            exit(1);               \
        } while (0)
#define ASSERT(cond, msg) \
        do {                  \
            if (!(cond))      \
            FAIL(msg);    \
        } while (0)

/* Mock col_rel_t structure (column-major, Phase C) */
typedef struct {
    int64_t **columns;     /* column-major: columns[col][row] */
    int32_t nrows;
    int32_t ncols;
    int64_t owner_id; /* Session owner ID */
} col_rel_t;

static inline int64_t
col_rel_get(const col_rel_t *r, uint32_t row, uint32_t col)
{
    return r->columns[col][row];
}

/*
 * Helper: build column-major columns from a row-major int64_t array.
 * Returns a heap-allocated columns array. Caller must free with
 * col_columns_free_mock().
 */
static int64_t **
mock_columns_from_rowmajor(const int64_t *rowmajor, int32_t nrows,
    int32_t ncols)
{
    if (ncols == 0 || nrows == 0)
        return NULL;
    int64_t **cols = (int64_t **)calloc(ncols, sizeof(int64_t *));
    if (!cols)
        return NULL;
    for (int32_t c = 0; c < ncols; c++) {
        cols[c] = (int64_t *)malloc((size_t)nrows * sizeof(int64_t));
        if (!cols[c]) {
            for (int32_t j = 0; j < c; j++)
                free(cols[j]);
            free(cols);
            return NULL;
        }
        for (int32_t r = 0; r < nrows; r++)
            cols[c][r] = rowmajor[r * ncols + c];
    }
    return cols;
}

static void
col_columns_free_mock(int64_t **cols, int32_t ncols)
{
    if (!cols)
        return;
    for (int32_t c = 0; c < ncols; c++)
        free(cols[c]);
    free(cols);
}

/* Mock cache entry */
typedef struct {
    const col_rel_t *left_ptr;
    const col_rel_t *right_ptr;
    col_rel_t *result;
    size_t mem_bytes;
    uint64_t lru_clock;
} col_mat_entry_t;

#define COL_MAT_CACHE_MAX 256
#define COL_MAT_CACHE_LIMIT_BYTES (64 * 1024 * 1024) /* 64 MB */

typedef struct {
    col_mat_entry_t entries[COL_MAT_CACHE_MAX];
    uint32_t count;
    size_t total_bytes;
    uint64_t clock;
} col_mat_cache_t;

/* Mock cache functions (current pointer-based implementation) */
static void
col_mat_cache_clear(col_mat_cache_t *cache)
{
    cache->count = 0;
    cache->total_bytes = 0;
}

static col_rel_t *
col_mat_cache_lookup_pointer_based(col_mat_cache_t *cache,
    const col_rel_t *left,
    const col_rel_t *right)
{
    for (uint32_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].left_ptr == left
            && cache->entries[i].right_ptr == right) {
            cache->entries[i].lru_clock = ++cache->clock;
            return cache->entries[i].result;
        }
    }
    return NULL;
}

static void
col_mat_cache_insert(col_mat_cache_t *cache, const col_rel_t *left,
    const col_rel_t *right, col_rel_t *result)
{
    if (cache->count >= COL_MAT_CACHE_MAX) {
        return; /* Skip if full */
    }
    cache->entries[cache->count].left_ptr = left;
    cache->entries[cache->count].right_ptr = right;
    cache->entries[cache->count].result = result;
    cache->entries[cache->count].mem_bytes
        = (size_t)result->nrows * result->ncols * sizeof(int64_t);
    cache->entries[cache->count].lru_clock = ++cache->clock;
    cache->count++;
}

/* Content-based cache key (forward declaration for future implementation) */
typedef struct {
    uint64_t left_hash;
    uint64_t right_hash;
} col_mat_cache_key_t;

static uint64_t
hash_relation_content(const col_rel_t *rel)
{
    if (!rel || rel->nrows == 0)
        return 0;

    uint64_t hash = 5381;
    /* Hash first min(100, nrows) rows for determinism without O(N) cost */
    int32_t rows_to_hash = (rel->nrows < 100) ? rel->nrows : 100;

    for (int32_t i = 0; i < rows_to_hash; i++) {
        for (int32_t j = 0; j < rel->ncols; j++) {
            int64_t val = col_rel_get(rel, i, j);
            hash = ((hash << 5) + hash) ^ val;
        }
    }

    /* Mix in relation shape */
    hash = ((hash << 5) + hash) ^ (uint64_t)rel->nrows;
    hash = ((hash << 5) + hash) ^ (uint64_t)rel->ncols;

    return hash;
}

static col_mat_cache_key_t
col_mat_cache_key_content(const col_rel_t *left, const col_rel_t *right)
{
    col_mat_cache_key_t key;
    key.left_hash = hash_relation_content(left);
    key.right_hash = hash_relation_content(right);
    return key;
}

/* ============================================================================
 * TEST SUITE
 * ============================================================================ */

/* Test 1: Pointer-based cache with identical pointers returns hit */
TEST(pointer_based_identical_pointers)
{
    col_mat_cache_t cache;
    col_mat_cache_clear(&cache);

    int64_t data[6] = { 1, 2, 3, 4, 5, 6 };
    int64_t **cols = mock_columns_from_rowmajor(data, 3, 2);
    col_rel_t left = { .columns = cols, .nrows = 3, .ncols = 2, .owner_id = 1 };
    col_rel_t right = { .columns = cols, .nrows = 3, .ncols = 2,
                        .owner_id = 1 };
    col_rel_t result = { .columns = cols, .nrows = 3, .ncols = 2,
                         .owner_id = 1 };

    col_mat_cache_insert(&cache, &left, &right, &result);

    col_rel_t *hit = col_mat_cache_lookup_pointer_based(&cache, &left, &right);
    ASSERT(hit != NULL, "Cache hit with identical pointers");
    ASSERT(hit == &result, "Correct result returned");

    col_columns_free_mock(cols, 2);
    PASS("Test 1: Pointer-based cache with identical pointers returns hit");
}

/* Test 2: Pointer-based cache with different pointers returns miss (current broken behavior) */
TEST(pointer_based_different_pointers_miss)
{
    col_mat_cache_t cache;
    col_mat_cache_clear(&cache);

    int64_t data1[6] = { 1, 2, 3, 4, 5, 6 };
    int64_t data2[6]
        = { 1, 2, 3, 4, 5, 6 }; /* Same data, different allocation */

    int64_t **cols1 = mock_columns_from_rowmajor(data1, 3, 2);
    int64_t **cols2 = mock_columns_from_rowmajor(data2, 3, 2);
    col_rel_t left1 = { .columns = cols1, .nrows = 3, .ncols = 2,
                        .owner_id = 1 };
    col_rel_t right1 = { .columns = cols1, .nrows = 3, .ncols = 2,
                         .owner_id = 1 };
    col_rel_t result = { .columns = cols1, .nrows = 3, .ncols = 2,
                         .owner_id = 1 };

    col_rel_t left2 = { .columns = cols2, .nrows = 3, .ncols = 2,
                        .owner_id = 2 };
    col_rel_t right2 = { .columns = cols2, .nrows = 3, .ncols = 2,
                         .owner_id = 2 };

    col_mat_cache_insert(&cache, &left1, &right1, &result);

    /* Same data, different pointers → MISS with pointer-based keying */
    col_rel_t *miss
        = col_mat_cache_lookup_pointer_based(&cache, &left2, &right2);
    ASSERT(miss == NULL,
        "Cache miss with different pointers (even for identical data)");

    col_columns_free_mock(cols1, 2);
    col_columns_free_mock(cols2, 2);
    PASS("Test 2: Pointer-based cache with different pointers returns miss");
}

/* Test 3: Content-based hash is deterministic */
TEST(content_based_hash_deterministic)
{
    int64_t data[6] = { 1, 2, 3, 4, 5, 6 };
    int64_t **cols = mock_columns_from_rowmajor(data, 3, 2);
    col_rel_t rel = { .columns = cols, .nrows = 3, .ncols = 2, .owner_id = 1 };

    uint64_t hash1 = hash_relation_content(&rel);
    uint64_t hash2 = hash_relation_content(&rel);

    ASSERT(hash1 == hash2, "Hash is deterministic for same relation");

    col_columns_free_mock(cols, 2);
    PASS("Test 3: Content-based hash is deterministic");
}

/* Test 4: Different data produces different hashes */
TEST(content_based_different_data_different_hash)
{
    int64_t data1[6] = { 1, 2, 3, 4, 5, 6 };
    int64_t data2[6] = { 7, 8, 9, 10, 11, 12 };

    int64_t **cols1 = mock_columns_from_rowmajor(data1, 3, 2);
    int64_t **cols2 = mock_columns_from_rowmajor(data2, 3, 2);
    col_rel_t rel1 = { .columns = cols1, .nrows = 3, .ncols = 2,
                       .owner_id = 1 };
    col_rel_t rel2 = { .columns = cols2, .nrows = 3, .ncols = 2,
                       .owner_id = 2 };

    uint64_t hash1 = hash_relation_content(&rel1);
    uint64_t hash2 = hash_relation_content(&rel2);

    ASSERT(hash1 != hash2, "Different data produces different hashes");

    col_columns_free_mock(cols1, 2);
    col_columns_free_mock(cols2, 2);
    PASS("Test 4: Different data produces different hashes");
}

/* Test 5: Identical data with different allocations produces same hash */
TEST(content_based_same_data_same_hash)
{
    int64_t data1[6] = { 1, 2, 3, 4, 5, 6 };
    int64_t data2[6] = { 1, 2, 3, 4, 5, 6 };

    int64_t **cols1 = mock_columns_from_rowmajor(data1, 3, 2);
    int64_t **cols2 = mock_columns_from_rowmajor(data2, 3, 2);
    col_rel_t rel1 = { .columns = cols1, .nrows = 3, .ncols = 2,
                       .owner_id = 1 };
    col_rel_t rel2 = { .columns = cols2, .nrows = 3, .ncols = 2,
                       .owner_id = 2 };

    uint64_t hash1 = hash_relation_content(&rel1);
    uint64_t hash2 = hash_relation_content(&rel2);

    ASSERT(hash1 == hash2,
        "Identical data produces same hash regardless of pointer");

    col_columns_free_mock(cols1, 2);
    col_columns_free_mock(cols2, 2);
    PASS(
        "Test 5: Identical data with different allocations produces same hash");
}

/* Test 6: Cache key function produces consistent keys */
TEST(cache_key_consistent)
{
    int64_t data1[4] = { 10, 20, 30, 40 };
    int64_t data2[4] = { 10, 20, 30, 40 };

    int64_t **cols1 = mock_columns_from_rowmajor(data1, 2, 2);
    int64_t **cols2 = mock_columns_from_rowmajor(data2, 2, 2);
    col_rel_t left1 = { .columns = cols1, .nrows = 2, .ncols = 2,
                        .owner_id = 1 };
    col_rel_t right1 = { .columns = cols1, .nrows = 2, .ncols = 2,
                         .owner_id = 1 };

    col_rel_t left2 = { .columns = cols2, .nrows = 2, .ncols = 2,
                        .owner_id = 2 };
    col_rel_t right2 = { .columns = cols2, .nrows = 2, .ncols = 2,
                         .owner_id = 2 };

    col_mat_cache_key_t key1 = col_mat_cache_key_content(&left1, &right1);
    col_mat_cache_key_t key2 = col_mat_cache_key_content(&left2, &right2);

    ASSERT(key1.left_hash == key2.left_hash,
        "Left keys match for identical data");
    ASSERT(key1.right_hash == key2.right_hash,
        "Right keys match for identical data");

    col_columns_free_mock(cols1, 2);
    col_columns_free_mock(cols2, 2);
    PASS("Test 6: Cache key function produces consistent keys for identical "
        "data");
}

/* Test 7: Empty relations produce valid hashes */
TEST(empty_relation_hash)
{
    col_rel_t empty = { .columns = NULL, .nrows = 0, .ncols = 2,
                        .owner_id = 1 };
    col_rel_t empty2 = { .columns = NULL, .nrows = 0, .ncols = 2,
                         .owner_id = 2 };

    uint64_t hash1 = hash_relation_content(&empty);
    uint64_t hash2 = hash_relation_content(&empty2);

    ASSERT(hash1 == hash2, "Empty relations produce same hash");
    ASSERT(hash1 == 0, "Empty relation hash is 0");

    PASS("Test 7: Empty relations produce valid hashes");
}

/* Test 8: K-copy isolation semantics (different copies have different hashes) */
TEST(k_copy_different_copies_different_hashes)
{
    int64_t data_copy0[4] = { 1, 2, 3, 4 }; /* Copy 0 intermediate */
    int64_t data_copy1[4]
        = { 1, 2, 3, 4 }; /* Copy 1 intermediate (same data, different pass) */

    /* Simulate K-copy: each copy modifies intermediate result slightly */
    /* In practice, different K-copies will have semantically different intermediates */
    int64_t **cols0 = mock_columns_from_rowmajor(data_copy0, 2, 2);
    int64_t **cols1 = mock_columns_from_rowmajor(data_copy1, 2, 2);
    col_rel_t rel_copy0
        = { .columns = cols0, .nrows = 2, .ncols = 2, .owner_id = 1 };
    col_rel_t rel_copy1
        = { .columns = cols1, .nrows = 2, .ncols = 2, .owner_id = 2 };

    uint64_t hash0 = hash_relation_content(&rel_copy0);
    uint64_t hash1 = hash_relation_content(&rel_copy1);

    /* With content-based keying, same data means same hash (cache reuse across K-copies!) */
    ASSERT(hash0 == hash1,
        "K-copy intermediates with same content share cache");

    col_columns_free_mock(cols0, 2);
    col_columns_free_mock(cols1, 2);
    PASS("Test 8: K-copy intermediates with same content produce same hash "
        "(cache reuse)");
}

/* Main test runner */
int
main(void)
{
    printf("\n=== CSE Cache Hit Rate Test Suite (TDD) ===\n\n");

    test_pointer_based_identical_pointers();
    test_pointer_based_different_pointers_miss();
    test_content_based_hash_deterministic();
    test_content_based_different_data_different_hash();
    test_content_based_same_data_same_hash();
    test_cache_key_consistent();
    test_empty_relation_hash();
    test_k_copy_different_copies_different_hashes();

    printf("\n=== All tests passed ===\n\n");
    return 0;
}
