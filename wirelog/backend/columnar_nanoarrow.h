/*
 * backend/columnar_nanoarrow.h - wirelog Nanoarrow Columnar Backend
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * INTERNAL HEADER - not installed, not part of public API.
 *
 * ========================================================================
 * Overview
 * ========================================================================
 *
 * The nanoarrow columnar backend stores relations in row-major int64_t
 * buffers and uses Apache Arrow schemas (via nanoarrow) for type metadata.
 * Evaluation uses a stack-based relational algebra interpreter executing
 * the wl_plan_t operator sequence with semi-naive fixed-point
 * iteration for recursive strata.
 *
 * ========================================================================
 * Evaluation Model
 * ========================================================================
 *
 * Plan operators are emitted in post-order (left child first), forming a
 * stack machine:
 *
 *   VARIABLE(rel)   -> push named relation onto eval stack
 *   MAP(indices)    -> pop, project columns, push result
 *   FILTER(expr)    -> pop, apply predicate, push filtered result
 *   JOIN(right,keys)-> pop left, join with named right, push output
 *   ANTIJOIN(...)   -> pop left, remove rows matching right, push
 *   CONCAT          -> pop top two, concatenate, push union
 *   CONSOLIDATE     -> pop, sort+deduplicate, push
 *   REDUCE(agg)     -> pop, group-by + aggregate, push
 *   SEMIJOIN(...)   -> pop left, semijoin with right, push left cols only
 *
 * Column name tracking: each stack entry carries column names for
 * variable→position resolution in JOIN conditions.
 */

#ifndef WL_BACKEND_COLUMNAR_NANOARROW_H
#define WL_BACKEND_COLUMNAR_NANOARROW_H

#include "../backend.h"
#include "../exec_plan.h"

/* ======================================================================== */
/* K-Fusion Metadata                                                        */
/* ======================================================================== */

/**
 * wl_plan_op_k_fusion_t:
 *
 * Backend-specific metadata for a WL_PLAN_OP_K_FUSION operator.
 * Stored in wl_plan_op_t.opaque_data and owned by the plan.
 *
 * A K_FUSION operator encapsulates K independent operator sequences
 * (one per semi-naive delta copy) for parallel workqueue execution.
 * Each sequence in k_ops[d] is annotated with appropriate delta_mode
 * values: position d uses FORCE_DELTA; all other IDB positions use
 * FORCE_FULL.
 *
 * @k:          Number of delta copies (>= 2).
 * @k_ops:      Array of K operator sequence pointers (each owned here).
 * @k_op_counts: Number of operators in each sequence k_ops[d].
 */
typedef struct {
    uint32_t k;
    wl_plan_op_t **k_ops;
    uint32_t *k_op_counts;
} wl_plan_op_k_fusion_t;

/*
 * NOTE: wl_col_session_t and COL_SESSION() are defined in columnar_nanoarrow.c
 * because col_rel_t (a private implementation type) cannot be declared in this
 * header. See columnar_nanoarrow.c for the full memory layout documentation.
 *
 * Summary of the embedding contract:
 *   - wl_col_session_t embeds wl_session_t as its first field (base)
 *   - (wl_col_session_t *)session is safe per C11 §6.7.2.1 ¶15
 *   - session.c:38 sets (*out)->backend after col_session_create returns
 *   - All col_session_* vtable functions cast via COL_SESSION() internally
 *
 * @see backend_dd.c:35-44 for the embedding pattern reference
 * @see session.h:38-40 for canonical wl_session_t definition
 */

/**
 * col_delta_timestamp_t - Per-row provenance record for delta tracking.
 *
 * Attached to rows in delta relations produced by col_eval_stratum().
 * Records when and where each row was first derived during semi-naive
 * fixed-point evaluation.  Used for debugging row lineage and by Phase 3C
 * frontier tracking.
 *
 * Fields:
 *   iteration  Fixed-point iteration (0-based) that first produced this row.
 *   stratum    Stratum index within the evaluation plan (0-based).
 *   worker     K-fusion worker index (0 = sequential / non-parallel path).
 *   _reserved  Must be zero (reserved for future use).
 */
typedef struct {
    uint32_t iteration;
    uint32_t stratum;
    uint32_t worker;
    uint32_t _reserved;
} col_delta_timestamp_t;

/**
 * col_session_get_iteration_count:
 *
 * Return the number of fixed-point iterations performed during the last
 * evaluation.  Returns 0 if no evaluation has occurred yet.
 *
 * @param sess  A wl_session_t* backed by the columnar backend.
 */
uint32_t
col_session_get_iteration_count(wl_session_t *sess);

#endif /* WL_BACKEND_COLUMNAR_NANOARROW_H */
