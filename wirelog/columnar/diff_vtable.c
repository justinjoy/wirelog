/*
 * columnar/diff_vtable.c - Differential frontier vtable implementation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Issue #262: Implements the differential frontier model via the
 * col_frontier_ops_t vtable interface. Unlike the epoch vtable, reset
 * preserves the iteration (convergence point) so that bulk-insert
 * does not destroy previously computed frontiers.
 */

#include "columnar/internal.h"

#include <stdint.h>

static bool
diff_should_skip_iteration(void *arg, uint32_t stratum_idx,
    uint32_t eff_iter)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (stratum_idx >= MAX_STRATA)
        return false;
    bool valid = (sess->frontiers[stratum_idx].iteration != UINT32_MAX);
    bool beyond = (eff_iter > sess->frontiers[stratum_idx].iteration);
    return valid && beyond;
}

static bool
diff_should_skip_rule(void *arg, uint32_t rule_id, uint32_t eff_iter)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (rule_id >= MAX_RULES)
        return false;
    bool valid = (sess->rule_frontiers[rule_id].iteration != UINT32_MAX);
    bool beyond = (eff_iter > sess->rule_frontiers[rule_id].iteration);
    return valid && beyond;
}

static void
diff_record_stratum_convergence(void *arg, uint32_t stratum_idx,
    uint32_t outer_epoch, uint32_t iteration)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (stratum_idx >= MAX_STRATA)
        return;
    sess->frontiers[stratum_idx].outer_epoch = outer_epoch;
    sess->frontiers[stratum_idx].iteration = iteration;
}

static void
diff_record_rule_convergence(void *arg, uint32_t rule_id,
    uint32_t outer_epoch, uint32_t iteration)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (rule_id >= MAX_RULES)
        return;
    sess->rule_frontiers[rule_id].outer_epoch = outer_epoch;
    sess->rule_frontiers[rule_id].iteration = iteration;
}

static void
diff_reset_stratum_frontier(void *arg, uint32_t stratum_idx,
    uint32_t outer_epoch)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (stratum_idx >= MAX_STRATA)
        return;
    sess->frontiers[stratum_idx].outer_epoch = outer_epoch;
    /* Differential: preserve iteration (don't set UINT32_MAX) */
}

static void
diff_reset_rule_frontier(void *arg, uint32_t rule_id,
    uint32_t outer_epoch)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (rule_id >= MAX_RULES)
        return;
    sess->rule_frontiers[rule_id].outer_epoch = outer_epoch;
    /* Differential: preserve iteration (don't set UINT32_MAX) */
}

static void
diff_init_stratum(void *arg, uint32_t stratum_idx)
{
    wl_col_session_t *sess = (wl_col_session_t *)arg;
    if (stratum_idx >= MAX_STRATA)
        return;
    if (sess->frontiers[stratum_idx].iteration == 0)
        sess->frontiers[stratum_idx].iteration = UINT32_MAX;
}

const col_frontier_ops_t col_frontier_diff_ops = {
    .should_skip_iteration = diff_should_skip_iteration,
    .should_skip_rule = diff_should_skip_rule,
    .record_stratum_convergence = diff_record_stratum_convergence,
    .record_rule_convergence = diff_record_rule_convergence,
    .reset_stratum_frontier = diff_reset_stratum_frontier,
    .reset_rule_frontier = diff_reset_rule_frontier,
    .init_stratum = diff_init_stratum,
};
