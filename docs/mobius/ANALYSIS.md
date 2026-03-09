# Möbius Inversion in wirelog: Missing Implementation Analysis

**Date**: 2026-03-09
**Status**: Research Phase
**Related PRs**: Phase 3B (Timestamped Delta Tracking), Phase 4 (Incremental Evaluation)

---

## Executive Summary

wirelog's current aggregation implementation in `col_op_reduce()` does **not** apply Möbius inversion, which is the mathematical foundation of Differential Dataflow's correctness for incremental computation. This gap prevents accurate handling of signed deltas (insertions vs deletions) in aggregate functions.

**Key Finding**: The aggregation logic treats all updates as positive (+1), but Differential Dataflow uses **weighted sets (Z-sets)** where each element carries a signed multiplicity. Proper handling requires Möbius inversion over the iteration lattice.

---

## Background: Three Concepts (From provided articles)

### 1. Arrow-for-Timely: Bimodal Batch Distribution
- **Bulk loads** (1M rows) → columnar (Arrow) ✅ *Implemented in Phase 2C*
- **Incremental updates** (1-100 rows) → row-based ✅ *Using timestamps*
- **Compaction boundary**: Format conversion happens once at merge point

**wirelog Status**: ✅ nanoarrow columnar storage in place

---

### 2. Timely Protocol: 5-Layer Stack
```
L1: Wire layer (UDP/DPDK)           ❌ Not implemented
L2: Channel layer (multiplexing)    ❌ Not implemented
L3: Progress layer (frontier)       🔄 Phase 3D: In progress
    └─ Frontier tracking for iteration skip
L4: Delta Batch layer (Arrow)       ✅ Partially implemented
    └─ col_delta_timestamp_t exists but not used in aggregation
L5: Dataflow binding (evaluator)    ✅ col_eval_stratum()
```

**wirelog Status**: L3-L5 partially implemented, L4 **incomplete**

---

### 3. Differential Dataflow: Weighted Sets + Möbius Inversion
```
Weighted Sets (Z-sets):
  - Each element carries a weight: +1 (insertion), -1 (deletion)
  - Time becomes multi-dimensional: (external_epoch, internal_iteration)
  - Collections = "accumulated differences over time"

Möbius Inversion:
  - Mathematical technique for computing differences accurately
  - Essential for aggregating signed deltas across multi-dimensional time
```

**wirelog Status**: ❌ **NOT IMPLEMENTED**

---

## The Missing Piece: Weighted Sets in Aggregation

### Current Implementation (col_op_reduce)

```c
// File: wirelog/backend/columnar_nanoarrow.c:2922
col_op_reduce(const wl_plan_op_t *op, eval_stack_t *stack) {
    // ...

    // Line 2970: Update aggregate
    switch (op->agg_fn) {
    case WIRELOG_AGG_COUNT:
        orow[gc]++;  // ← Always +1, never -1
        break;
    case WIRELOG_AGG_SUM:
        orow[gc] += val;  // ← Assumes val is always positive
        break;
    // ...
    }
}
```

**Problem**: Each input row is treated as having implicit multiplicity +1. There is no way to express:
- A deletion (multiplicity -1)
- A correction (multiplicity -5, then +3)
- A retraction (multiplicity 0)

### What's Missing: Z-Set Tracking

Differential Dataflow would track:

```c
// What should be in col_rel_t (but isn't):
typedef struct {
    int64_t *data;           // tuple values
    uint32_t nrows;

    // ← MISSING: Multiplicities
    int64_t *multiplicities; // weight per row: {-3, +5, -1, ...}

    // ← MISSING: Multi-dimensional time
    col_delta_timestamp_t *timestamps;  // (epoch, iteration, stratum)
} col_rel_weighted_t;
```

---

## Where Möbius Inversion is Needed

### Scenario: Recursive COUNT with Deletions

**Iteration 0 (seed)**:
```
Input: tc(1,2), tc(2,3)
COUNT by first element:
  1 → count=1
  2 → count=1
```

**Iteration 1 (incremental addition)**:
```
Delta: tc(1,3) [new derivation]
COUNT by first element:
  1 → count=2 (1→2 and 1→3)
```

**Iteration 2 (retraction - error correction)**:
```
Delta: tc(1,3) with multiplicity -1  [retracting the error]
COUNT by first element:
  1 → should become count=1 again (only 1→2 remains)
```

**Current wirelog behavior**:
```
Iteration 2: orow[gc]++;  // ← Still adds, doesn't subtract
Result: count becomes 2 (WRONG!)
```

**Correct behavior (with multiplicities)**:
```
Iteration 2: orow[gc] += delta_multiplicity;  // orow += -1
Result: count becomes 1 (CORRECT!)
```

---

## Where Möbius Inversion Applies

### 1. **Aggregation with Negative Multiplicities**

When computing aggregates over a multi-dimensional lattice of (epoch, iteration):

```
Aggregate(t_outer, t_inner) = Σ [multiplicity × value]

For deletion: multiplicity = -1
For insertion: multiplicity = +1
```

Möbius inversion solves:
- Given: Aggregate at certain (t_outer, t_inner)
- Find: Aggregate at a different (t_outer, t_inner)
- Via: Möbius function μ on the poset of timestamps

**In wirelog**: Needed in Phase 3B when timestamped deltas carry signs.

---

### 2. **Delta Propagation Through JOIN**

```
JOIN(L, R) with multiplicities:

If L has row {a, b} with mult -2
And R has row {b, c} with mult +3
Then result has {a, b, c} with mult (-2 × +3) = -6
```

This is a **multiplication of weights**, not a sum. The formula involves:
- Each input's weight
- Möbius function on the join lattice

**In wirelog**: Needed when col_op_join handles signed deltas.

---

### 3. **Correctness of Recursive Aggregation**

In recursive strata (like transitive closure with COUNT):

```
WL = { reachable(X) as list of targets, count each unique X }

Iteration 0: reachable(1) = [2, 3], count(1) = 2
Iteration 1: reachable(1) += [4], count(1) = 3
Iteration 2: retract [3] with mult -1, count(1) should = 2

Without Möbius inversion:
  count(1) = 2 + 1 - 1 = 2 ✓ (happens to work)

With complex multi-way joins:
  Multiple derivation paths, conflicting updates
  Need Möbius inversion to resolve correctly
```

---

## Current Timestamp Tracking (Incomplete)

wirelog has **partial infrastructure** for timestamped deltas:

```c
// Line 61 in columnar_nanoarrow.c:
typedef struct {
    char *name;
    uint32_t ncols;
    int64_t *data;
    uint32_t nrows;

    col_delta_timestamp_t *timestamps;  // ← Exists but...
} col_rel_t;

// Line 3551:
delta->timestamps[ti].iteration = iter;
delta->timestamps[ti].stratum = stratum_idx;
```

**What's tracking**:
- ✅ Iteration number per row
- ✅ Stratum index per row

**What's missing**:
- ❌ Multiplicity per row (sign: +1 or -1)
- ❌ Möbius inversion logic over the lattice
- ❌ Proper aggregation with signed weights

---

## Phase-by-Phase Implementation Plan

### Phase 3B: Timestamped Delta Tracking (**CURRENT TARGET**)

**Add multiplicity tracking**:
```c
// Extend col_delta_timestamp_t
typedef struct {
    uint32_t epoch;        // External time
    uint32_t iteration;    // Internal iteration
    uint32_t stratum;      // Stratum index
    int64_t multiplicity;  // ← NEW: Weight of this row
} col_delta_timestamp_t;
```

**Update col_op_reduce**:
```c
// Instead of: orow[gc]++;
// Do: int64_t mult = src_row_multiplicity(in, r);
//     orow[gc] += mult;  // Signed aggregation
```

**Expected ROI**: 3-5x speedup on incremental DOOP

---

### Phase 3C: Arrangement + JOIN Multiplicities

**Arrange collections by (epoch, iteration, key)**:
```c
typedef struct {
    int64_t *data;           // tuple values
    int64_t *multiplicities; // weight per row
    col_delta_timestamp_t *timestamps;
    uint32_t nrows;
} col_arrangement_t;
```

**JOIN with multiplicities**:
```c
// result_mult = left_mult × right_mult
void col_op_join_weighted(col_arrangement_t *left,
                          col_arrangement_t *right) {
    for (left_row in left):
        for (right_row in right):
            result_mult = left->mult[i] * right->mult[j];
            append(result, left[i] + right[j], result_mult);
}
```

---

### Phase 3D: Möbius Inversion for Frontier Skip

**When can we skip an iteration?**

If all remaining deltas have multiplicity 0 at a frontier point:
```c
bool can_skip_iteration(col_frontier_t *frontier, uint32_t iter) {
    // For each stratum S:
    //   If Σ(multiplicity of rows in S at iteration iter) == 0
    //   Then S cannot produce new facts
    //   → Skip via Möbius inversion over the iteration poset

    return frontier->net_multiplicity == 0;
}
```

---

## Möbius Function for Iteration Lattice

The iteration lattice is a **total order**: 0 < 1 < 2 < ... < MAX_ITER

For a total order, the Möbius function is:
```
μ(i, j) = {  1   if i = j
          { -1   if i < j
          {  0   if i > j
```

**Application to wirelog**:

Given aggregates at iteration levels:
```
A(0) = 5      (facts at iteration 0)
A(1) = 8      (facts at iteration 1)
A(2) = 10     (facts at iteration 2)
```

The deltas (things newly derived) are:
```
Δ(0) = A(0) = 5
Δ(1) = A(1) - A(0) = 3  [μ(0,1) × A(0) + μ(1,1) × A(1)]
Δ(2) = A(2) - A(1) = 2  [μ(0,2) × A(0) + μ(1,2) × A(1) + μ(2,2) × A(2)]
```

In wirelog:
```c
// Möbius inversion for deltas:
int64_t compute_delta(col_rel_t *iter_i, col_rel_t *iter_i_minus_1) {
    // Δ(i) = A(i) - A(i-1)
    // = μ(i-1, i) × A(i-1) + μ(i, i) × A(i)
    // = -1 × A(i-1) + 1 × A(i)
    // = A(i) - A(i-1)

    return count_in(iter_i) - count_in(iter_i_minus_1);
}
```

---

## Code Locations for Implementation

### Files that need modification:

1. **columnar_nanoarrow.h**: Extend col_delta_timestamp_t with multiplicity
2. **columnar_nanoarrow.c**:
   - Line ~2922: `col_op_reduce()` → handle signed aggregates
   - Line ~2200: `col_op_join()` → multiply multiplicities
   - Line ~3540: `col_session_insert_incremental()` → track deltas with signs
3. **exec_plan_gen.c**: Plan generation for weighted deltas
4. **session.h**: Session API for multiplicity tracking

---

## Why This Matters

**Without Möbius inversion**:
- ❌ Cannot correctly retract facts (deletions always treated as insertions)
- ❌ Aggregates become incorrect on incremental updates
- ❌ Gap to DD performance remains (91x)

**With Möbius inversion**:
- ✅ Correct incremental semantics (Differential Dataflow equivalence)
- ✅ Proper delta propagation through joins and aggregates
- ✅ Frontier skip optimization becomes sound
- ✅ Path to DD parity performance (47s on DOOP)

---

## References

- **Arrow-for-Timely**: https://groou.com/research/2026/02/22/arrow-for-timely-dataflow/
- **Timely Protocol**: https://groou.com/essay/research/2026/02/17/timely-dataflow-protocol/
- **Differential Dataflow**: https://groou.com/essay/ai/2026/02/14/differential-dataflow/
- **Naiad Paper**: "Naiad: A Timely Dataflow System" (SOSP 2013)
- **Differential Dataflow Book**: https://github.com/frankmcsherry/differential-dataflow

---

## Next Steps

1. ✅ Document the gap (this file)
2. 📋 Design Phase 3B: Multiplicity tracking in col_rel_t
3. 📋 Implement Möbius inversion logic
4. 📋 Validation: Compare incremental aggregates against DD oracle
5. 📋 Performance: Measure 3-5x improvement target
