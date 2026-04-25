/*
 * col_rel_deep_copy_fixture.h - Reusable test fixtures for deep-copy validation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Issue #556: deep-copy validation framework.
 *
 * Lightweight helpers shared by tests that exercise col_rel_deep_copy.
 * Each test using these helpers should #include this header and link the
 * matching col_rel_deep_copy_fixture.c into the same test executable.
 */

#ifndef WL_COL_REL_DEEP_COPY_FIXTURE_H
#define WL_COL_REL_DEEP_COPY_FIXTURE_H

#include "../wirelog/columnar/internal.h"

/*
 * deep_copy_fixture_make_relation:
 *   Build a heap-owned col_rel_t with deterministic content for deep-copy
 *   validation tests.  Caller owns the result and must col_rel_destroy() it.
 *
 *   Each row r and column c is filled with the value
 *       columns[c][r] = (int64_t)((r * ncols) + c)
 *   so independence checks (mutate copy, verify source) have predictable
 *   patterns regardless of (nrows, ncols).
 *
 *   Column names are "col_0", "col_1", ..., "col_{ncols-1}".
 *
 * Returns NULL on allocation failure, otherwise a fully populated relation.
 */
col_rel_t *
deep_copy_fixture_make_relation(const char *name, uint32_t nrows,
    uint32_t ncols);

/*
 * deep_copy_fixture_assert_design_invariants:
 *   Assert the design invariants R-1, R-2, R-3 hold on a deep-copy
 *   destination relation:
 *     pool_owned == false, arena_owned == false, mem_ledger == NULL,
 *     col_shared == NULL, dedup_slots == NULL, dedup_cap == 0,
 *     dedup_count == 0, row_scratch == NULL.
 *
 *   On failure prints which invariant was violated to stderr.
 *
 * Returns 1 on pass, 0 on fail.
 */
int
deep_copy_fixture_assert_design_invariants(const col_rel_t *dst);

/*
 * deep_copy_fixture_assert_relations_equal:
 *   Assert two relations have identical observable content:
 *     ncols, nrows, capacity, sorted_nrows, base_nrows match;
 *     columns[c][r] match for all c in [0, ncols), r in [0, nrows);
 *     col_names[c] strings match (string-equal, pointers must differ
 *       when both relations own their col_names array);
 *     name strings match;
 *     compound_kind, compound_count match;
 *     compound_arity_map deeply equal where present;
 *     has_graph_column, graph_col_idx match;
 *     run_count, run_ends match;
 *     schema_ok matches.
 *
 *   Does NOT assert pointer identity for the top-level columns array;
 *   in fact REJECTS pointer aliasing for owned arrays (columns,
 *   columns[c], col_names, col_names[c], name, compound_arity_map).
 *
 *   On failure prints the first observed mismatch to stderr.
 *
 * Returns 1 on pass, 0 on fail.
 */
int
deep_copy_fixture_assert_relations_equal(const col_rel_t *a,
    const col_rel_t *b);

#endif /* WL_COL_REL_DEEP_COPY_FIXTURE_H */
