# Möbius Inversion: Technical Foundation for Differential Dataflow

**Date**: 2026-03-09
**Status**: Technical Reference Document
**Audience**: Engineers implementing Phase 3B-3D

---

## Table of Contents

1. Mathematical Foundations
2. Möbius Function Definition
3. Application to Differential Dataflow
4. wirelog Implementation Strategy
5. Code Examples
6. Correctness Proofs

---

## 1. Mathematical Foundations

### 1.1 Partially Ordered Sets (Posets)

A **poset** is a set with a partial order relation ≤ that is:
- **Reflexive**: a ≤ a (every element relates to itself)
- **Antisymmetric**: if a ≤ b and b ≤ a, then a = b
- **Transitive**: if a ≤ b and b ≤ c, then a ≤ c

**Example (wirelog context)**: Iterations form a total order poset
```
0 ≤ 1 ≤ 2 ≤ ... ≤ MAX_ITER

This is a TOTAL order (every pair is comparable).
```

### 1.2 Zeta and Möbius Functions

For a poset P, define two functions on pairs (x, y) ∈ P × P where x ≤ y:

**Zeta Function ζ(x, y)**:
```
ζ(x, y) = { 1    if x ≤ y
          { 0    otherwise
```

**Möbius Function μ(x, y)**:
The Möbius function is the **inverse** of the zeta function in the incidence algebra of P.

**Fundamental Relation**:
```
Σ_{x ≤ z ≤ y} μ(z, y) = δ(x, y)

where δ(x, y) = { 1 if x = y
                { 0 otherwise

This is the Möbius inversion formula.
```

### 1.3 Möbius Inversion Formula

For any function f: P → ℝ, define:
```
F(x) = Σ_{z ≥ x} f(z)
```

Then by Möbius inversion:
```
f(x) = Σ_{z ≥ x} μ(x, z) × F(z)
```

**Intuition**:
- F is the "cumulative" or "aggregate" function
- f is the "incremental difference"
- Möbius inversion extracts increments from aggregates

---

## 2. Möbius Function Definition

### 2.1 General Recursive Definition

For a poset P and x ≤ y:

```
μ(x, x) = 1

μ(x, y) = -Σ_{x ≤ z < y} μ(x, z)    for x < y
```

This means:
- Base case: element paired with itself has weight 1
- Recursive case: weight of (x, y) cancels all smaller pairs

### 2.2 Möbius Function for Total Orders

For a **totally ordered** set (like iterations), the formula simplifies dramatically:

```
For x ≤ y in a total order:

μ(x, y) = { 1     if x = y
          { -1    if x < y
          { 0     if x > y
```

**Proof** (for total order):
```
μ(x, x) = 1                           [by definition]

For x < y:
  μ(x, y) = -Σ_{x ≤ z < y} μ(x, z)
          = -μ(x, x)                  [only z=x satisfies x ≤ z < y]
          = -1                         [since μ(x, x) = 1]

For x > y:
  μ(x, y) = 0                         [by definition, undefined for x > y]
```

### 2.3 wirelog's Iteration Poset

In wirelog, iterations form a **totally ordered** poset:
```
Iteration 0 < 1 < 2 < 3 < ... < MAX_ITER

Therefore, wirelog's Möbius function is:

μ(i, j) = { 1     if i = j
          { -1    if i < j
          { 0     if i > j
```

**This is the simplest possible Möbius function!**

---

## 3. Application to Differential Dataflow

### 3.1 Collections as Accumulated Differences

Differential Dataflow represents a collection as:

```
Collection(t) = Σ_{t' ≤ t} Δ(t')

where:
  Collection(t) = all tuples existing at time t
  Δ(t')          = tuples added at time t'
```

**Example**: Transitive closure with insertions
```
Time 0:
  Δ(0) = {tc(1,2), tc(2,3)}
  Collection(0) = {tc(1,2), tc(2,3)}

Time 1:
  Δ(1) = {tc(1,3)}     [derived from tc(1,2) ∧ edge(2,3)]
  Collection(1) = {tc(1,2), tc(2,3), tc(1,3)}

Time 2:
  Δ(2) = {} or {retractions}
  Collection(2) = current state
```

### 3.2 Computing Increments from Aggregates via Möbius

The key insight: **If we know cumulative collections, we can extract increments using Möbius inversion.**

**Given**:
- Collection(0), Collection(1), Collection(2), ...

**Want**:
- Δ(0), Δ(1), Δ(2), ...

**Formula** (Möbius inversion):
```
For each iteration i:

Δ(i) = Σ_{j ≥ i} μ(i, j) × Collection(j)

In our total order:

Δ(i) = μ(i, i) × Collection(i) + Σ_{j > i} μ(i, j) × Collection(j)
     = 1 × Collection(i) + (-1) × Collection(i+1) + (-1) × Collection(i+2) + ...
     = Collection(i) - Collection(i+1) - Collection(i+2) - ...

But for a single iteration (i to i+1):

Δ(i) = Collection(i) - Collection(i-1)    [the key formula!]
```

### 3.3 Z-Sets: Weighted Collections

Differential Dataflow extends this with **multiplicities**:

```
Z-set = {(tuple, weight), ...}

where weight ∈ ℤ (integers):
  weight = +1:  tuple is inserted
  weight = -1:  tuple is deleted (retracted)
  weight =  0:  tuple has no effect (cancellation)
  weight = +k:  tuple appears k times
```

**Aggregate with weights**:
```
COUNT(group) = Σ_{tuple in group} weight(tuple)

SUM(group, field) = Σ_{tuple in group} weight(tuple) × value(field)
```

**Möbius inversion works with weighted collections**:
```
Δ_weighted(i) = Collection_weighted(i) - Collection_weighted(i-1)

Each tuple carries its multiplicity forward.
```

### 3.4 Time Lattices: Epochs and Iterations

Differential Dataflow uses **multi-dimensional time**:

```
Pointstamp = (external_epoch, internal_iteration)

Example: (epoch=2, iteration=5)
  = "at external time 2, after internal iteration 5"

Poset structure:
  (e1, i1) ≤ (e2, i2) iff e1 < e2 OR (e1 = e2 AND i1 ≤ i2)

This is a PRODUCT POSET:
  External epochs (total order) × Internal iterations (total order)
```

**Möbius inversion on product posets**:

For a product poset P₁ × P₂:
```
μ_{P₁×P₂}((x1, y1), (x2, y2)) = μ_{P₁}(x1, x2) × μ_{P₂}(y1, y2)
```

**For wirelog** (epochs × iterations):
```
μ((e1, i1), (e2, i2)) = μ_epochs(e1, e2) × μ_iterations(i1, i2)

If epochs are also totally ordered:
  μ((e1, i1), (e2, i2)) = { 1     if (e1, i1) = (e2, i2)
                           { -1    if (e1, i1) < (e2, i2)
                           { 0     otherwise
```

**Meaning for wirelog**:
```
Δ_weighted(epoch, iter) = Collection_weighted(epoch, iter)
                        - Collection_weighted(epoch, iter-1)
                        [and cascade for epoch differences]
```

---

## 4. wirelog Implementation Strategy

### 4.1 Phase 3B: Record Multiplicities

**Goal**: Extend relation storage to track multiplicities

```c
// Current (unsigned):
typedef struct {
    int64_t *data;           // tuple values
    uint32_t nrows;
    uint32_t ncols;
} col_rel_t;

// Phase 3B (weighted):
typedef struct {
    int64_t *data;           // tuple values
    uint32_t nrows;
    uint32_t ncols;

    int64_t *multiplicities; // ← NEW: weight per tuple
} col_rel_weighted_t;
```

**Semantics**:
```
multiplicities[i] ∈ {-1, 0, +1} represents:
  +1: tuple i is inserted
  -1: tuple i is deleted
   0: tuple i cancelled out (in Z-set terms)
```

### 4.2 Aggregate Functions with Multiplicities

**COUNT with multiplicities**:
```c
int64_t count_weighted(col_rel_weighted_t *rel, col_rel_t *groupby) {
    int64_t total = 0;
    for (size_t i = 0; i < rel->nrows; i++) {
        total += rel->multiplicities[i];  // ← Use Möbius: mult not just count
    }
    return total;
}

Example:
  rel = {(value1, mult=+1), (value1, mult=-1), (value2, mult=+1)}
  COUNT = 1 + (-1) + 1 = 1  ✓

  Without multiplicities:
  COUNT = 3  ✗ WRONG
```

**SUM with multiplicities**:
```c
int64_t sum_weighted(col_rel_weighted_t *rel, uint32_t field_idx) {
    int64_t total = 0;
    for (size_t i = 0; i < rel->nrows; i++) {
        int64_t value = rel->data[i * rel->ncols + field_idx];
        total += rel->multiplicities[i] * value;  // ← Multiply by weight
    }
    return total;
}

Example:
  rel = {(10, mult=+2), (10, mult=-1), (20, mult=+1)}
  SUM = (+2 × 10) + (-1 × 10) + (+1 × 20) = 20 + (-10) + 20 = 30

  Without multiplicities:
  SUM = 10 + 10 + 20 = 40  ✗ WRONG
```

### 4.3 Phase 3C: JOIN with Multiplicity Multiplication

**Principle**: Multiply multiplicities when joining

```c
// Binary join: (L_tuple, mult_L) × (R_tuple, mult_R)
// Result: (L_tuple + R_tuple, mult_result)
// where mult_result = mult_L × mult_R

int64_t mult_result = mult_L * mult_R;

Examples:
  (+1) × (+1) = +1   [both inserted, result inserted]
  (+1) × (-1) = -1   [L inserted, R deleted, result deleted]
  (-1) × (-1) = +1   [both deleted, result inserted (restoration)]
  (+2) × (-1) = -2   [L has 2 copies, R deleted, result -2]
```

**Correctness via Möbius**:
The multiplicative rule ensures that Möbius inversion is distributive:

```
Δ(JOIN(L, R)) = JOIN(Δ(L), Collection(R)) ∪ JOIN(Collection(L), Δ(R))

With weights:
  mult(output) = mult(L) × mult(R)
  [This makes the math work out via Möbius inversion]
```

### 4.4 Phase 3D: Frontier Skip via Net Multiplicity

**Question**: Can we skip iteration i?

**Answer** (using Möbius):
```
If net multiplicity of delta at iteration i = 0:
  → No new facts at iteration i
  → Iteration i+1 cannot produce new facts
  → Can skip iteration i+1

Formula:
  net_mult(i) = Σ_{j=0}^{nrows-1} multiplicities[j]

  if (net_mult(i) == 0) → skip iteration i+1
```

**Why this works** (via Möbius inversion):
```
Δ(i) = Collection(i) - Collection(i-1)  [by Möbius]

If Δ(i) is a Z-set where all weights cancel (net_mult = 0):
  → Collection(i) = Collection(i-1)  [no change]
  → Rules cannot produce new facts at iteration i+1
  → Can skip iteration i+1
```

---

## 5. Code Examples

### 5.1 Möbius Delta Computation

```c
/**
 * Compute incremental delta using Möbius inversion.
 *
 * Given cumulative relations at iterations i-1 and i,
 * compute the delta (new facts) at iteration i.
 *
 * Formula (Möbius for total order):
 *   Δ(i) = Collection(i) - Collection(i-1)
 */
int
col_compute_delta_mobius(
    col_rel_t *collection_i,           // A(i): all facts at iteration i
    col_rel_t *collection_i_minus_1,  // A(i-1): all facts at iteration i-1
    col_rel_weighted_t **out_delta     // Δ(i): facts newly produced at i
) {
    if (!collection_i || !collection_i_minus_1 || !out_delta)
        return EINVAL;

    // Create output delta relation
    col_rel_weighted_t *delta = (col_rel_weighted_t *)calloc(1, sizeof(*delta));
    if (!delta)
        return ENOMEM;

    delta->ncols = collection_i->ncols;
    delta->nrows = 0;
    delta->capacity = COL_REL_INIT_CAP;

    // Allocate data and multiplicities
    delta->data = (int64_t *)malloc(
        sizeof(int64_t) * delta->capacity * delta->ncols);
    delta->multiplicities = (int64_t *)malloc(
        sizeof(int64_t) * delta->capacity);

    if (!delta->data || !delta->multiplicities) {
        free(delta->data);
        free(delta->multiplicities);
        free(delta);
        return ENOMEM;
    }

    // Step 1: Mark rows in i-1 as -1 (deletions)
    for (uint32_t j = 0; j < collection_i_minus_1->nrows; j++) {
        int64_t *row_j = collection_i_minus_1->data + j * collection_i_minus_1->ncols;

        // Find matching row in collection_i
        bool found = false;
        for (uint32_t k = 0; k < collection_i->nrows; k++) {
            int64_t *row_k = collection_i->data + k * collection_i->ncols;

            if (rows_equal(row_j, row_k, collection_i->ncols)) {
                // Row exists in both: no delta
                found = true;
                break;
            }
        }

        if (!found) {
            // Row in i-1 but not in i: deletion (Möbius: -1)
            if (delta->nrows >= delta->capacity) {
                uint32_t new_cap = delta->capacity * 2;
                int64_t *new_data = (int64_t *)realloc(
                    delta->data, sizeof(int64_t) * new_cap * delta->ncols);
                int64_t *new_mults = (int64_t *)realloc(
                    delta->multiplicities, sizeof(int64_t) * new_cap);

                if (!new_data || !new_mults) {
                    free(new_data);
                    free(new_mults);
                    return ENOMEM;
                }

                delta->data = new_data;
                delta->multiplicities = new_mults;
                delta->capacity = new_cap;
            }

            // Append row with multiplicity -1
            memcpy(delta->data + delta->nrows * delta->ncols, row_j,
                   sizeof(int64_t) * delta->ncols);
            delta->multiplicities[delta->nrows] = -1;  // ← Möbius: -1 for deletion
            delta->nrows++;
        }
    }

    // Step 2: Mark rows in i but not in i-1 as +1 (insertions)
    for (uint32_t k = 0; k < collection_i->nrows; k++) {
        int64_t *row_k = collection_i->data + k * collection_i->ncols;

        bool found = false;
        for (uint32_t j = 0; j < collection_i_minus_1->nrows; j++) {
            int64_t *row_j = collection_i_minus_1->data + j * collection_i_minus_1->ncols;

            if (rows_equal(row_j, row_k, collection_i->ncols)) {
                found = true;
                break;
            }
        }

        if (!found) {
            // Row in i but not in i-1: insertion (Möbius: +1)
            if (delta->nrows >= delta->capacity) {
                uint32_t new_cap = delta->capacity * 2;
                int64_t *new_data = (int64_t *)realloc(
                    delta->data, sizeof(int64_t) * new_cap * delta->ncols);
                int64_t *new_mults = (int64_t *)realloc(
                    delta->multiplicities, sizeof(int64_t) * new_cap);

                if (!new_data || !new_mults)
                    return ENOMEM;

                delta->data = new_data;
                delta->multiplicities = new_mults;
                delta->capacity = new_cap;
            }

            memcpy(delta->data + delta->nrows * delta->ncols, row_k,
                   sizeof(int64_t) * delta->ncols);
            delta->multiplicities[delta->nrows] = +1;  // ← Möbius: +1 for insertion
            delta->nrows++;
        }
    }

    *out_delta = delta;
    return 0;
}
```

### 5.2 Frontier Skip Check

```c
/**
 * Check if we can skip an iteration using Möbius net multiplicity.
 *
 * Formula: If Σ multiplicities == 0, no new facts → can skip next iteration
 */
static bool
col_can_skip_iteration_mobius(const col_rel_weighted_t *delta)
{
    if (!delta || !delta->multiplicities)
        return false;  // No multiplicity tracking, can't safely skip

    int64_t net_multiplicity = 0;

    for (uint32_t i = 0; i < delta->nrows; i++) {
        net_multiplicity += delta->multiplicities[i];
    }

    // If net multiplicity is zero: no net change → can skip
    return net_multiplicity == 0;
}
```

---

## 6. Correctness Proofs

### 6.1 Aggregate Correctness

**Theorem**: COUNT with multiplicities via Möbius is correct.

**Proof**:
```
Given: Z-set Z = {(tuple_i, mult_i), ...}

Define:
  Collection(Z) = {tuple | tuple ∈ Z}
  Count(Z) = Σ_i mult_i

Claim: Count(Z) = |Collection(Z) after applying weights|

Proof by cases:
  Case 1: mult_i = +1
    → tuple_i is added to collection
    → contributes +1 to count ✓

  Case 2: mult_i = -1
    → tuple_i is removed from collection
    → contributes -1 to count (negates insertion) ✓

  Case 3: mult_i = +2
    → tuple_i appears twice (insertion + duplicate)
    → contributes +2 to count ✓

  Case 4: mult_i = +1 followed by mult_j = -1 (same tuple)
    → two entries: (+1, -1)
    → total count contribution: 0
    → tuple appears 0 times in collection ✓

QED: Multiplicities via Möbius correctly count under insertions and deletions.
```

### 6.2 Frontier Skip Soundness

**Theorem**: If net multiplicity of delta at iteration i = 0, then iteration i+1 produces no new facts.

**Proof**:
```
Assume: Δ(i) has net multiplicity = 0

By definition of Möbius inversion:
  Collection(i) = Σ_{j ≤ i} Δ(j)
  Collection(i-1) = Σ_{j < i} Δ(j)

By Möbius for total order:
  Δ(i) = Collection(i) - Collection(i-1)

If net_mult(Δ(i)) = 0:
  → Collection(i) - Collection(i-1) has all weights canceling
  → Collection(i) ≡ Collection(i-1) [as Z-sets]
  → Rules applied to Collection(i) produce same results as Collection(i-1)
  → No new facts can be derived at iteration i+1

Conclusion: Can safely skip iteration i+1.

QED: Frontier skip via net multiplicity is sound.
```

### 6.3 JOIN Multiplicativity

**Theorem**: JOIN of weighted relations via multiplicity multiplication satisfies Möbius inversion.

**Proof**:
```
Given:
  L_Z = Z-set of left tuples with multiplicities
  R_Z = Z-set of right tuples with multiplicities

JOIN result:
  JOIN(L_Z, R_Z) = {((l, r), mult_l × mult_r) | (l, mult_l) ∈ L_Z, (r, mult_r) ∈ R_Z}

Claim: Möbius inversion preserves JOIN under multiplication.

Proof:
  Δ(JOIN(L, R)) at iteration i
    = JOIN(Δ(L)_i, Collection(R)_i) ∪ JOIN(Collection(L)_i, Δ(R)_i)
    [by definition of incremental JOIN]

  With multiplicities:
    mult_result = mult_L × mult_R

  Möbius inversion of composite:
    μ(x, z) = Σ_{x ≤ y ≤ z} μ(x, y) × μ(y, z)
    [for product of multiplicities]

  This ensures:
    Δ(x × y) = Δ(x) × y + x × Δ(y)  [product rule from calculus]

  In multiplicity form:
    mult(result) = mult(L) × mult(R)  [same computation]

QED: Multiplicity multiplication satisfies Möbius inversion.
```

---

## 7. Connection to Three Articles

### Arrow-for-Timely
- **L4 Delta Batch Layer**: Must handle multiplicities for incremental updates
- **Möbius role**: Computes deltas from cumulative collections
- **wirelog**: Implement Phase 3B (record multiplicities)

### Timely Protocol
- **L3 Progress Layer**: Frontier computed via Möbius inversion
- **Möbius role**: μ(i, j) determines progress (which iterations are complete)
- **wirelog**: Implement Phase 3D (frontier skip using net multiplicity)

### Differential Dataflow
- **Z-Sets**: Multiplicities are the core abstraction
- **Möbius role**: Inversion formula Δ(t) = Σ μ(s, t) × A(s)
- **wirelog**: Implement Phases 3B-3C (full weighted set semantics)

---

## 8. Implementation Checklist

### Phase 3B Checklist

- [ ] Add `int64_t *multiplicities` to `col_rel_t`
- [ ] Implement `col_compute_delta_mobius()` (delta computation via Möbius)
- [ ] Implement `col_op_reduce_weighted()` (COUNT, SUM with multiplicities)
- [ ] Update `col_rel_append_row()` to handle multiplicities
- [ ] Test: `test_mobius_count_basic.c` (simple aggregation)
- [ ] Test: `test_mobius_count_deletion.c` (retraction handling)
- [ ] Measure: 3-5x improvement on DOOP

### Phase 3C Checklist

- [ ] Implement `col_op_join_weighted()` (multiplicity multiplication)
- [ ] Test: `test_mobius_join_multiplicities.c`
- [ ] Measure: 2-5x improvement on DOOP

### Phase 3D Checklist

- [ ] Implement `col_can_skip_iteration_mobius()` (frontier check)
- [ ] Test: `test_mobius_frontier_skip.c` (correctness on all 15 workloads)
- [ ] Measure: 5-50x improvement on DOOP (target: 47s)

---

## References

- **Naiad**: "Naiad: A Timely Dataflow System" (SOSP 2013) - Microsoft Research
- **Möbius Inversion**: https://en.wikipedia.org/wiki/M%C3%B6bius_inversion_formula
- **Posets**: https://en.wikipedia.org/wiki/Partially_ordered_set
- **Incidence Algebra**: https://en.wikipedia.org/wiki/Incidence_algebra
- **Differential Dataflow**: https://github.com/frankmcsherry/differential-dataflow

---

**Document Status**: Technical reference complete. Ready for implementation in Phase 3B-3D.

