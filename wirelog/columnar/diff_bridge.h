/*
 * columnar/diff_bridge.h - Differential bridge: epoch <-> lattice translation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * INTERNAL HEADER - not installed, not part of public API.
 *
 * Bidirectional translation between col_frontier_2d_t (epoch model)
 * and col_diff_trace_t (lattice model) for differential dataflow.
 */

#ifndef WL_COLUMNAR_DIFF_BRIDGE_H
#define WL_COLUMNAR_DIFF_BRIDGE_H

#include "columnar/columnar_nanoarrow.h" /* col_frontier_2d_t */
#include "columnar/diff_trace.h"         /* col_diff_trace_t  */

/* Epoch -> Lattice: (epoch, iter) -> (epoch, iter, worker=0, reserved=0) */
static inline col_diff_trace_t
col_diff_bridge_epoch_to_trace(const col_frontier_2d_t *f2d)
{
    return col_diff_trace_init(f2d->outer_epoch, f2d->iteration, 0);
}

/* Lattice -> Epoch: (epoch, iter, worker, reserved) -> (epoch, iter) */
static inline col_frontier_2d_t
col_diff_bridge_trace_to_epoch(const col_diff_trace_t *trace)
{
    col_frontier_2d_t f2d = {
        .outer_epoch = trace->outer_epoch,
        .iteration = trace->iteration,
    };
    return f2d;
}

/* Verify lattice ordering is preserved across translation */
static inline bool
col_diff_bridge_ordering_preserved(const col_frontier_2d_t *a,
    const col_frontier_2d_t *b)
{
    col_diff_trace_t ta = col_diff_bridge_epoch_to_trace(a);
    col_diff_trace_t tb = col_diff_bridge_epoch_to_trace(b);
    /* epoch ordering must match trace ordering */
    int epoch_cmp = (a->outer_epoch < b->outer_epoch)  ? -1 :
        (a->outer_epoch > b->outer_epoch)  ? 1 :
        (a->iteration < b->iteration)      ? -1 :
        (a->iteration > b->iteration)      ? 1 : 0;
    int trace_cmp = col_diff_trace_compare(&ta, &tb);
    return (epoch_cmp < 0 && trace_cmp < 0) ||
           (epoch_cmp == 0 && trace_cmp == 0) ||
           (epoch_cmp > 0 && trace_cmp > 0);
}

#endif /* WL_COLUMNAR_DIFF_BRIDGE_H */
