# Möbius Inversion Implementation Design for wirelog

**Date**: 2026-03-09
**Phase**: 3B (Timestamped Delta Tracking)
**Status**: Design Document
**Author**: Analysis from Arrow-for-Timely + Timely Protocol + Differential Dataflow concepts

---

## 1. Type System Changes

### Current State

```c
// columnar_nanoarrow.h
typedef struct {
    int64_t *data;
    uint32_t nrows;
    uint32_t ncols;
    uint32_t capacity;

    // Timestamp: iteration when row was produced
    col_delta_timestamp_t *timestamps;  // Tracks (epoch, iteration, stratum)
} col_rel_t;

// Individual row's metadata
typedef struct {
    uint32_t epoch;
    uint32_t iteration;
    uint32_t stratum;
    // ← MISSING: multiplicity field
} col_delta_timestamp_t;
```

### Required Changes

```c
// Step 1: Add multiplicity field to timestamp
typedef struct {
    uint32_t epoch;
    uint32_t iteration;
    uint32_t stratum;
    int64_t multiplicity;  // ← NEW: +1 (insert), -1 (delete), 0 (retract)
} col_delta_timestamp_t_v2;

// Step 2: Add weighted Z-set support to relation
typedef struct {
    int64_t *data;
    uint32_t nrows;
    uint32_t ncols;
    uint32_t capacity;

    // Original timestamps (iteration tracking)
    col_delta_timestamp_t_v2 *timestamps;

    // ← NEW: Z-set multiplicities (enable weighted sets)
    // NULL = all weights are +1 (unweighted relation)
    // Non-NULL = weights per row (weighted Z-set)
    int64_t *multiplicities;

    // Flag: is this relation a Z-set?
    bool is_weighted;
} col_rel_t_v2;

// Step 3: Track deltas with signs
typedef struct {
    col_rel_t_v2 *relation;
    int64_t *delta_values;      // For FILTER/MAP: actual values
    int64_t *delta_mults;        // ← NEW: Multiplicities of deltas
    uint32_t delta_nrows;
} col_delta_t_v2;
```

---

## 2. Core Operations: Möbius Inversion

### 2.1 Delta Computation (Iteration Boundary)

**Problem**: How do we compute Δ(i) = changes at iteration i?

**Solution**: Use Möbius inversion over the total order of iterations.

```c
/*
 * Möbius function for total order (iterations):
 *   μ(i, j) = { 1    if i == j
 *             { -1   if i < j
 *             { 0    if i > j
 *
 * This means: Δ(i) = A(i) - A(i-1)
 *            where A(i) = cumulative aggregate at iteration i
 */

// Phase 3B: Compute delta with signs
int64_t col_compute_delta_multiplicities(
    col_rel_t *curr_iter,      // A(i)
    col_rel_t *prev_iter,      // A(i-1)
    int64_t **out_mults        // Δ_mult(i)
) {
    uint32_t delta_count = 0;

    // Initialize output
    *out_mults = (int64_t *)calloc(curr_iter->nrows, sizeof(int64_t));
    if (!*out_mults)
        return -ENOMEM;

    // Δ_mult(i) = μ(i-1, i) × A(i-1) + μ(i, i) × A(i)
    //           = -1 × A(i-1) + 1 × A(i)

    // Mark rows in prev_iter as -1
    for (uint32_t j = 0; j < prev_iter->nrows; j++) {
        // Find matching row in curr_iter
        bool found = false;
        for (uint32_t k = 0; k < curr_iter->nrows; k++) {
            if (rows_equal(prev_iter->data[j], curr_iter->data[k])) {
                (*out_mults)[k] += -1;  // Subtract from prev
                found = true;
                break;
            }
        }
        if (!found) {
            // Row existed in prev but not curr (was deleted)
            (*out_mults)[j] = -1;
            delta_count++;
        }
    }

    // Mark rows in curr_iter (not in prev) as +1
    for (uint32_t k = 0; k < curr_iter->nrows; k++) {
        bool found = false;
        for (uint32_t j = 0; j < prev_iter->nrows; j++) {
            if (rows_equal(prev_iter->data[j], curr_iter->data[k])) {
                found = true;
                break;
            }
        }
        if (!found) {
            (*out_mults)[k] = +1;  // New in curr
            delta_count++;
        }
    }

    return delta_count;
}
```

---

### 2.2 Aggregation with Multiplicities

**Current (wrong)**:
```c
case WIRELOG_AGG_COUNT:
    orow[gc]++;  // Always +1, never -1
    break;
```

**Fixed (with Möbius)**:
```c
// Phase 3B: col_op_reduce with weighted Z-sets
static int
col_op_reduce_weighted(const wl_plan_op_t *op, eval_stack_t *stack)
{
    eval_entry_t e = eval_stack_pop(stack);
    if (!e.rel)
        return EINVAL;

    col_rel_t *in = e.rel;
    uint32_t gc = op->group_by_count;
    uint32_t ocols = gc + 1;

    col_rel_t *out = col_rel_new_auto("$reduce", ocols);
    if (!out)
        return ENOMEM;

    // For weighted sets, also allocate multiplicities
    if (in->multiplicities) {
        out->multiplicities = (int64_t *)calloc(out->capacity, sizeof(int64_t));
        if (!out->multiplicities) {
            col_rel_free_contents(out);
            free(out);
            return ENOMEM;
        }
    }

    // Aggregation loop
    for (uint32_t r = 0; r < in->nrows; r++) {
        const int64_t *row = in->data + r * in->ncols;

        // Get multiplicity of input row (Möbius: +1 or -1)
        int64_t row_mult = in->multiplicities ? in->multiplicities[r] : 1;

        // Find matching group in output
        bool found = false;
        for (uint32_t o = 0; o < out->nrows; o++) {
            int64_t *orow = out->data + o * ocols;
            bool match = true;
            for (uint32_t k = 0; k < gc && match; k++) {
                uint32_t gi = op->group_by_indices ? op->group_by_indices[k] : k;
                match = (row[gi < in->ncols ? gi : 0] == orow[k]);
            }

            if (match) {
                // Update with signed multiplicity
                int64_t val = (in->ncols > gc) ? row[gc] : 1;

                switch (op->agg_fn) {
                case WIRELOG_AGG_COUNT:
                    orow[gc] += row_mult;  // ← Signed! +1 or -1
                    if (out->multiplicities)
                        out->multiplicities[o] += row_mult;
                    break;

                case WIRELOG_AGG_SUM:
                    orow[gc] += val * row_mult;  // ← Multiply by multiplicity
                    if (out->multiplicities)
                        out->multiplicities[o] += row_mult;
                    break;

                // MIN/MAX: only apply if multiplicity is +1 (insertion)
                case WIRELOG_AGG_MIN:
                    if (row_mult > 0 && val < orow[gc])
                        orow[gc] = val;
                    break;

                case WIRELOG_AGG_MAX:
                    if (row_mult > 0 && val > orow[gc])
                        orow[gc] = val;
                    break;
                }
                found = true;
                break;
            }
        }

        if (!found) {
            // New group
            int64_t *tmp = (int64_t *)malloc(sizeof(int64_t) * ocols);
            if (!tmp) {
                if (in->multiplicities)
                    free(out->multiplicities);
                col_rel_free_contents(out);
                free(out);
                return ENOMEM;
            }

            for (uint32_t k = 0; k < gc; k++) {
                uint32_t gi = op->group_by_indices ? op->group_by_indices[k] : k;
                tmp[k] = row[gi < in->ncols ? gi : 0];
            }

            int64_t init_val = (in->ncols > gc) ? row[gc] : 1;
            tmp[gc] = (op->agg_fn == WIRELOG_AGG_COUNT) ? row_mult : (init_val * row_mult);

            col_rel_append_row(out, tmp);

            // Set multiplicity
            if (out->multiplicities)
                out->multiplicities[out->nrows - 1] = row_mult;

            free(tmp);
        }
    }

    if (e.owned) {
        col_rel_free_contents(in);
        free(in);
    }
    return eval_stack_push(stack, out, true);
}
```

---

### 2.3 JOIN with Multiplicity Multiplication

**Principle**: When joining two weighted relations, multiply their multiplicities.

```
JOIN(L with mult +3, R with mult -2) = (L×R) with mult (+3 × -2) = -6
```

**Implementation**:

```c
// Phase 3C: col_op_join with weighted Z-sets
static int
col_op_join_weighted(const wl_plan_op_t *op, eval_stack_t *stack)
{
    // Pop right (inner), then left (outer)
    eval_entry_t right_e = eval_stack_pop(stack);
    eval_entry_t left_e = eval_stack_pop(stack);

    col_rel_t *left = left_e.rel;
    col_rel_t *right = right_e.rel;

    if (!left || !right)
        return EINVAL;

    // Create result
    uint32_t lcols = left->ncols, rcols = right->ncols;
    uint32_t jcols = op->join_key_count;
    uint32_t ocols = lcols + rcols - jcols;  // De-duplicate join keys

    col_rel_t *out = col_rel_new_auto("$join", ocols);
    if (!out)
        return ENOMEM;

    // Allocate multiplicities if either input is weighted
    bool has_weights = left->multiplicities || right->multiplicities;
    if (has_weights) {
        out->multiplicities = (int64_t *)calloc(out->capacity, sizeof(int64_t));
        if (!out->multiplicities) {
            col_rel_free_contents(out);
            free(out);
            return ENOMEM;
        }
    }

    int64_t *tmp = (int64_t *)malloc(sizeof(int64_t) * ocols);
    if (!tmp) {
        if (has_weights)
            free(out->multiplicities);
        col_rel_free_contents(out);
        free(out);
        return ENOMEM;
    }

    // Nested loop join with multiplicity multiplication
    for (uint32_t l = 0; l < left->nrows; l++) {
        const int64_t *lrow = left->data + l * lcols;
        int64_t left_mult = left->multiplicities ? left->multiplicities[l] : 1;

        for (uint32_t r = 0; r < right->nrows; r++) {
            const int64_t *rrow = right->data + r * rcols;
            int64_t right_mult = right->multiplicities ? right->multiplicities[r] : 1;

            // Check join keys match
            bool match = true;
            for (uint32_t k = 0; k < jcols && match; k++) {
                uint32_t li = op->left_key_indices ? op->left_key_indices[k] : k;
                uint32_t ri = op->right_key_indices ? op->right_key_indices[k] : k;
                match = (lrow[li < lcols ? li : 0] == rrow[ri < rcols ? ri : 0]);
            }

            if (match) {
                // Build output row
                uint32_t ti = 0;

                // Copy non-key columns from left
                for (uint32_t i = 0; i < lcols; i++) {
                    bool is_key = false;
                    for (uint32_t k = 0; k < jcols; k++) {
                        uint32_t li = op->left_key_indices ? op->left_key_indices[k] : k;
                        if (i == li) {
                            is_key = true;
                            break;
                        }
                    }
                    if (!is_key)
                        tmp[ti++] = lrow[i];
                }

                // Copy all from right (includes keys)
                for (uint32_t i = 0; i < rcols; i++)
                    tmp[ti++] = rrow[i];

                // Append with multiplied multiplicity
                int rc = col_rel_append_row(out, tmp);
                if (rc != 0) {
                    free(tmp);
                    if (has_weights)
                        free(out->multiplicities);
                    col_rel_free_contents(out);
                    free(out);
                    return rc;
                }

                // Multiply multiplicities (Möbius principle)
                if (out->multiplicities) {
                    out->multiplicities[out->nrows - 1] = left_mult * right_mult;
                }
            }
        }
    }

    free(tmp);
    if (left_e.owned) {
        col_rel_free_contents(left);
        free(left);
    }
    if (right_e.owned) {
        col_rel_free_contents(right);
        free(right);
    }
    return eval_stack_push(stack, out, true);
}
```

---

### 2.4 Frontier Skip with Net Multiplicity

**Principle**: Skip an iteration if all deltas in a stratum sum to zero (no net change).

```c
// Phase 3D: Frontier skip using Möbius net multiplicity
static bool
col_can_skip_iteration(const wl_plan_relation_t *rplan,
                       col_rel_t *delta,
                       uint32_t iter)
{
    if (!delta || !delta->multiplicities)
        return false;  // Can't skip if no multiplicity tracking

    // Compute net multiplicity
    int64_t net_mult = 0;
    for (uint32_t i = 0; i < delta->nrows; i++) {
        net_mult += delta->multiplicities[i];
    }

    // If net multiplicity is zero: no facts produced at this iteration
    // → Can skip via Möbius inversion (frontier.iteration > current_iter)
    return net_mult == 0;
}
```

---

## 3. Integration Points

### 3.1 Session API Changes

```c
// In wl_col_session_t:
typedef struct {
    // ... existing fields ...

    // ← NEW: Enable Z-set tracking
    bool track_multiplicities;

    // ← NEW: Möbius inversion strategy
    enum {
        MOBIUS_NONE,         // Unweighted sets (current)
        MOBIUS_SIGNS,        // Track +1/-1 only
        MOBIUS_FULL          // Full multiplicities (future)
    } mobius_mode;
} wl_col_session_t;
```

### 3.2 Session Insert with Multiplicities

```c
// Phase 3B: col_session_insert_incremental with signs
int
col_session_insert_incremental_v2(
    wl_col_session_t *sess,
    const char *rel_name,
    const int64_t *row,
    uint32_t ncols,
    int64_t multiplicity  // ← NEW: +1 (insert), -1 (delete)
) {
    // ... validate relation exists ...

    col_rel_t *r = find_relation(sess, rel_name);

    // Append to EDB delta
    col_rel_append_row(r, row);

    // Record multiplicity
    if (r->multiplicities) {
        r->multiplicities[r->nrows - 1] = multiplicity;
    }

    return 0;
}
```

---

## 4. Correctness Properties

### Invariant 1: Aggregation Linearity

```
For any two disjoint input sets I1, I2:
  Aggregate(I1 ∪ I2) = Aggregate(I1) + Aggregate(I2)

With multiplicities:
  Aggregate({row:+3, row:-2}) = +3 - 2 = +1 ✓
```

### Invariant 2: Join Multiplicativity

```
For weighted relations:
  Mult(JOIN(L, R)) = Mult(L) × Mult(R)

Example:
  (a:+2) JOIN (b:-3) = (a,b):-6 ✓
```

### Invariant 3: Möbius Delta Correctness

```
For total order of iterations:
  Δ(i) = A(i) - A(i-1)  [via Möbius]

Verification:
  If A(0)=5, A(1)=8, A(2)=10:
  Δ(0) = 5, Δ(1) = 3, Δ(2) = 2
  Σ Δ(i) = 10 = A(2) ✓
```

---

## 5. Validation Strategy

### Test 1: Simple COUNT with Deletion

```c
void test_mobius_count_deletion() {
    // Insert: (group1, val1), (group1, val2)
    session_insert(sess, "facts", {1, 1}, 2, +1);
    session_insert(sess, "facts", {1, 2}, 2, +1);

    // Query: COUNT by group
    result = session_snapshot(sess);
    // Expected: group 1 → count = 2
    assert(result[0].count == 2);

    // Delete one: retract (group1, val2)
    session_insert(sess, "facts", {1, 2}, 2, -1);

    result = session_snapshot(sess);
    // Expected: group 1 → count = 1 (with Möbius: 2 + (-1) = 1)
    assert(result[0].count == 1);
}
```

### Test 2: JOIN with Signed Multiplicities

```c
void test_mobius_join_multiplicities() {
    // L: (a:+2)
    // R: (b:-3)
    // JOIN result: (a,b):-6

    col_rel_t *left = ...;   // (a), mult=+2
    col_rel_t *right = ...;  // (b), mult=-3

    col_rel_t *joined = col_op_join_weighted(left, right);

    // Check multiplicity: +2 × -3 = -6
    assert(joined->multiplicities[0] == -6);
}
```

### Test 3: Incremental TC with Möbius

```c
void test_mobius_incremental_tc() {
    // Start: tc = {(1,2), (2,3)}
    // Iter 1 delta: +tc(1,3)  [mult: +1]
    // Iter 2 delta: -tc(1,3)  [mult: -1, retraction]

    // After iter 1: tc = {(1,2), (2,3), (1,3)}
    // After iter 2: tc = {(1,2), (2,3)}  [back to start]

    // Verify via snapshot comparison
}
```

---

## 6. Performance Impact

### Memory Overhead

```
Per-relation Z-set tracking:
  + int64_t multiplicity per row
  + one boolean (is_weighted)

  Example: 1M rows → +8MB per relation
  DOOP (6-8 relations): ~50-60MB overhead
  Acceptable for Phase 3B
```

### CPU Overhead

```
Aggregation loop:
  Current: orow[gc]++           [1 memory access]
  Weighted: orow[gc] += mult    [2 memory accesses]

  JOIN loop:
  Current: append(L[i] × R[j])            [1 append]
  Weighted: append with mult=L[i] × R[j]  [1 append + 1 mult]

  Expected: <5% CPU overhead in Phase 3B
```

---

## 7. Timeline

- **Week 1-2**: Extend col_delta_timestamp_t with multiplicity field
- **Week 2-3**: Implement col_op_reduce_weighted
- **Week 3-4**: Implement col_op_join_weighted
- **Week 4-5**: Implement Möbius delta computation
- **Week 5-6**: Regression testing + frontier skip optimization

**Target**: 3-5x improvement on DOOP (71m50s → 15-25m)

---

## References

- **Differential Dataflow**: Z-sets and partial orders (https://groou.com/essay/ai/2026/02/14/differential-dataflow/)
- **Möbius Function**: https://en.wikipedia.org/wiki/M%C3%B6bius_function
- **Naiad SOSP'13**: Pointstamps over lattices
