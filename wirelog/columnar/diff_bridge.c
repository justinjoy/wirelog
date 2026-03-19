/*
 * columnar/diff_bridge.c - Differential bridge implementation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Compile-time assertions for layout correctness of the epoch <-> lattice
 * translation bridge. All performance-critical functions are static inline
 * in diff_bridge.h.
 */

#include "columnar/diff_bridge.h"

#include <stddef.h>

#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
/* Verify col_frontier_2d_t and col_diff_trace_t share compatible layout
 * for the first two fields (outer_epoch, iteration). */
_Static_assert(
    offsetof(col_frontier_2d_t, outer_epoch) ==
    offsetof(col_diff_trace_t, outer_epoch),
    "outer_epoch offset must match between frontier_2d and diff_trace");

_Static_assert(
    offsetof(col_frontier_2d_t, iteration) ==
    offsetof(col_diff_trace_t, iteration),
    "iteration offset must match between frontier_2d and diff_trace");

_Static_assert(sizeof(col_frontier_2d_t) == 8,
    "col_frontier_2d_t must be exactly 8 bytes");

_Static_assert(sizeof(col_diff_trace_t) == 16,
    "col_diff_trace_t must be exactly 16 bytes");
#endif
#endif
