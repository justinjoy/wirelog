/*
 * test_diff_arrangement_deep_copy_buckets.c - deep-copy bucket sizing
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 */

#include "wirelog/columnar/diff_arrangement.h"

#include <stdint.h>
#include <stdio.h>

static int
expect(const char *name, int ok)
{
    if (ok) {
        printf("%s ... PASS\n", name);
        return 0;
    }
    printf("%s ... FAIL\n", name);
    return 1;
}

int
main(void)
{
    uint32_t key_cols[] = { 0 };
    col_diff_arrangement_t *arr
        = col_diff_arrangement_create(key_cols, 1, 0);
    if (!arr)
        return expect("create arrangement", 0);

    int failed = 0;
    failed += expect("grow creates more buckets than chain capacity",
            col_diff_arrangement_ensure_ht_capacity(arr, 8000) == 0
            && arr->nbuckets > arr->ht_cap);

    uint32_t bucket = arr->nbuckets - 1;
    arr->ht_head[bucket] = 1234;

    col_diff_arrangement_t *copy = col_diff_arrangement_deep_copy(arr);
    failed += expect("deep copy preserves high bucket head",
            copy && copy->nbuckets == arr->nbuckets
            && copy->ht_cap == arr->ht_cap
            && copy->ht_head[bucket] == 1234);

    col_diff_arrangement_destroy(copy);
    col_diff_arrangement_destroy(arr);
    return failed == 0 ? 0 : 1;
}
