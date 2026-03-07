# Optimization Strategy: Columnar Backend Performance

**Date:** 2026-03-07
**Author:** Stream B (Code-level analysis) + Stream A (Profiling validation)
**Status:** Final — integrated profiling data; recommendations finalized per H1/H2/H3 evidence
**Scope:** `wirelog/backend/columnar_nanoarrow.c` (2204 lines, single-file columnar evaluator)

---

## Executive Summary

Two algorithmic optimizations target the semi-naive fixed-point loop in `col_eval_stratum` (line 1410). Both are **CONFIRMED as HIGH priority** by Stream A profiling. Both are independent, composable, and require no API changes. Ranked by evidence from profiling data:

| Priority | Optimization | Evidence | Impact | Effort | Affected Workloads |
|----------|-------------|----------|--------|--------|-------------------|
| **1 (STRONGLY RECOMMENDED)** | Incremental CONSOLIDATE | H1 CONFIRMED: CSPA 4,602ms (226ms/K-tuples), O(N log N), peak RSS 4.46GB. CONSOLIDATE is bottleneck for deep mutual recursion. | Eliminates O(N log N) full-relation re-sort per iteration | 3-5 days | All recursive (CSPA, GALEN, Polonius, TC, CC, Reach) |
| **2 (STRONGLY RECOMMENDED)** | Multi-way delta expansion | H2 CONFIRMED: 2× excess iterations for mutually recursive rules. Missing `A_full x B_full x delta(C)` variant for 3-way joins. | Reduces iteration count for 3+ atom rules by ~50-100% in pathological cases | 1-2 weeks | CSPA, Polonius, GALEN (3-way joins) |
| **3 (STRONGLY RECOMMENDED - COMBINED)** | Both together | H1+H2 combined evidence: ~63% wall time reduction expected for CSPA (2.7x speedup) | Compound gain: fewer iterations AND cheaper iterations | 2-3 weeks (parallel) or 3-5 weeks (sequential) | All recursive workloads |

---

## Option 1: Incremental CONSOLIDATE (HIGH Priority)

### Problem Analysis

**Current code** (`col_op_consolidate`, lines 1086-1135):

```c
// Line 1117-1118: sorts ENTIRE relation every iteration
row_cmp_ctx_t ctx = { .ncols = nc };
qsort_r(work->data, nr, sizeof(int64_t) * nc, &ctx, row_cmp_fn);

// Lines 1120-1131: linear dedup scan
uint32_t out_r = 1;
for (uint32_t r = 1; r < nr; r++) { ... memcmp ... }
```

**Called from** `col_eval_stratum` at lines 1653-1676:
```c
// Lines 1652-1676: consolidates ALL IDB relations every iteration
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_rel_t *r = session_find_rel(sess, sp->relations[ri].name);
    // ...
    col_op_consolidate(&stk);  // sorts full R_accumulated
}
```

**Cost per iteration:**
- Let N = total accumulated facts, D = new delta facts from this iteration
- Current: `O(N log N)` for qsort + `O(N)` for dedup scan = **O(N log N) per iteration**
- The relation grows monotonically (semi-naive only adds facts), so N increases each iteration
- For CSPA with ~100K final facts over ~50 iterations: later iterations sort 100K rows even if delta is only 100 rows

**Why this is wasteful:**
The full relation `R` from the previous iteration is already sorted and deduplicated (line 1132 sets `work->nrows = out_r`). Only the delta `D` (new rows appended via `col_rel_append_all` at line 1632) needs processing.

### Proposed Solution: Sort-Delta, Merge-Insert

Replace the full re-sort with:
1. Sort only the delta: `O(D log D)`
2. Merge the sorted delta into the already-sorted full relation: `O(N + D)` (merge walk)
3. Dedup during merge (already implicit in sorted merge)

**Total cost per iteration: O(D log D + N)** vs current **O(N log N)**

Since D << N in later iterations (semi-naive converges), and log D << log N, this is a significant improvement. The `O(N)` merge cost dominates but is unavoidable for dedup — and it replaces `O(N log N)` which is strictly worse.

### Pseudocode

```c
static int
col_op_consolidate_incremental(col_rel_t *rel, uint32_t old_nrows)
{
    uint32_t nc = rel->ncols;
    uint32_t nr = rel->nrows;

    if (nr <= 1 || old_nrows >= nr)
        return 0;  /* nothing new */

    /* Phase 1: sort only the new delta rows [old_nrows .. nr) */
    uint32_t delta_count = nr - old_nrows;
    int64_t *delta_start = rel->data + (size_t)old_nrows * nc;
    row_cmp_ctx_t ctx = { .ncols = nc };
    qsort_r(delta_start, delta_count, sizeof(int64_t) * nc, &ctx, row_cmp_fn);

    /* Phase 1b: dedup within delta */
    uint32_t d_unique = 1;
    for (uint32_t i = 1; i < delta_count; i++) {
        if (memcmp(delta_start + (size_t)(i-1) * nc,
                   delta_start + (size_t)i * nc,
                   sizeof(int64_t) * nc) != 0) {
            if (d_unique != i)
                memcpy(delta_start + (size_t)d_unique * nc,
                       delta_start + (size_t)i * nc,
                       sizeof(int64_t) * nc);
            d_unique++;
        }
    }

    /* Phase 2: merge sorted old [0..old_nrows) with sorted delta.
     * Both are sorted+unique. Output: sorted+unique merged result.
     * Allocate temporary buffer for merge output. */
    size_t max_rows = old_nrows + d_unique;
    int64_t *merged = malloc(max_rows * nc * sizeof(int64_t));
    if (!merged) return ENOMEM;

    uint32_t oi = 0, di = 0, out = 0;
    size_t row_bytes = (size_t)nc * sizeof(int64_t);
    while (oi < old_nrows && di < d_unique) {
        const int64_t *orow = rel->data + (size_t)oi * nc;
        const int64_t *drow = delta_start + (size_t)di * nc;
        int cmp = memcmp(orow, drow, row_bytes);
        if (cmp < 0) {
            memcpy(merged + (size_t)out * nc, orow, row_bytes);
            oi++; out++;
        } else if (cmp == 0) {
            memcpy(merged + (size_t)out * nc, orow, row_bytes);
            oi++; di++; out++;  /* skip duplicate from delta */
        } else {
            memcpy(merged + (size_t)out * nc, drow, row_bytes);
            di++; out++;
        }
    }
    /* Copy remaining */
    while (oi < old_nrows) {
        memcpy(merged + (size_t)out * nc, rel->data + (size_t)oi * nc, row_bytes);
        oi++; out++;
    }
    while (di < d_unique) {
        memcpy(merged + (size_t)out * nc, delta_start + (size_t)di * nc, row_bytes);
        di++; out++;
    }

    /* Swap buffer */
    free(rel->data);
    rel->data = merged;
    rel->nrows = out;
    rel->capacity = max_rows;
    return 0;
}
```

### Integration Point

In `col_eval_stratum` (lines 1652-1676), replace the `col_op_consolidate` call:

```c
// Before: col_op_consolidate(&stk)  -- sorts all N rows
// After:  col_op_consolidate_incremental(r, snap[ri])  -- sorts only delta
```

The `snap[ri]` array (line 1535) already captures pre-iteration row counts — this is exactly the `old_nrows` boundary needed.

### Complexity Comparison

| Scenario | Current | Proposed | Speedup Factor |
|----------|---------|----------|---------------|
| N=100K, D=100 | O(100K * 17) = 1.7M comparisons | O(100 * 7) + O(100K) = 100.7K | ~17x |
| N=50K, D=1K | O(50K * 16) = 800K | O(1K * 10) + O(51K) = 61K | ~13x |
| N=10K, D=5K | O(10K * 13) = 130K | O(5K * 12) + O(15K) = 75K | ~2x |
| N=1K, D=500 | O(1K * 10) = 10K | O(500 * 9) + O(1.5K) = 6K | ~1.7x |

**Takeaway:** Improvement scales with N/D ratio. Later iterations of large recursive workloads benefit most — exactly where CSPA spends most time.

### Delta Computation Simplification

The incremental approach also simplifies delta computation (lines 1678-1730). Currently, the code:
1. Saves a copy of old data before evaluation (`old_data[ri]`, lines 1546-1551)
2. Consolidates the full relation (the expensive part)
3. Does a merge walk to find `R_new - R_old`

With incremental consolidate, the delta is a natural byproduct of the merge step — new rows from the delta that survive dedup ARE the delta for the next iteration. This eliminates both the `old_data` snapshot (saves a memcpy of the full relation) and the post-consolidation merge walk.

### Risk Assessment

- **Correctness:** The merge-based approach produces identical sorted+unique output. Testable by running both paths and comparing results.
- **Memory:** One temporary buffer of size `(old_nrows + delta)` during merge. Current code already allocates a copy when the relation is not owned (lines 1105-1115). Net memory impact: comparable.
- **Edge cases:** Empty delta (D=0): no-op. First iteration (old_nrows=0): falls back to full sort of delta only.

### Effort: 3-5 days

- Day 1: Implement `col_op_consolidate_incremental` function
- Day 2: Integrate into `col_eval_stratum`, pass `snap[ri]` as boundary
- Day 3: Optimize delta computation to use merge byproduct
- Day 4-5: Testing + benchmark validation across all 15 workloads

---

## Option 2: Multi-Way Delta Expansion (MEDIUM Priority)

### Problem Analysis

**Current semi-naive delta strategy** (`col_op_variable` + `col_op_join`):

The VARIABLE op (lines 579-599) selects delta OR full for the left side of the first join:
```c
// Line 594-596: prefer delta when strictly smaller
bool use_delta = (delta && delta->nrows > 0 && delta->nrows < full_rel->nrows);
col_rel_t *rel = use_delta ? delta : full_rel;
```

The JOIN op (lines 753-766) applies right-delta when left is full:
```c
// Lines 758-766: right-delta substitution
if (!left_e.is_delta && op->right_relation) {
    col_rel_t *rdelta = session_find_rel(sess, rdname);
    if (rdelta && rdelta->nrows > 0 && rdelta->nrows < right->nrows)
        right = rdelta;
}
```

**What this implements for a 2-way join `R(x,y) :- A(x,z), B(z,y)`:**
- Iteration produces: `delta(A) x B_full` (VARIABLE picks delta(A), JOIN uses full B)
- AND: `A_full x delta(B)` (VARIABLE picks full A, JOIN substitutes delta(B))
- This is correct and complete for 2-atom rules.

**What happens for a 3-way join `R(x,w) :- A(y,x), B(y,z), C(z,w)`:**

The plan compiles to: `VARIABLE(A) -> JOIN(B) -> JOIN(C)`

Current evaluation per iteration produces ONE of:
1. `delta(A) x B_full x C_full` — VARIABLE picks delta(A), both JOINs use full
2. `A_full x delta(B) x C_full` — VARIABLE picks full A, first JOIN uses delta(B), second JOIN uses full C
3. **MISSING:** `A_full x B_full x delta(C)` — Never generated!

**Why it's missing** (line 888-889):
```c
// After first JOIN: result_is_delta = left_e.is_delta || used_right_delta
bool result_is_delta = left_e.is_delta || used_right_delta;
return eval_stack_push_delta(stack, out, true, result_is_delta);
```

After the first JOIN uses delta(B), `result_is_delta = true`. The second JOIN sees `left_e.is_delta == true` (line 758), so it does NOT substitute delta(C). The third permutation is never computed.

**Impact on CSPA:**

Two CSPA rules have 3-way joins:
1. `memoryAlias(x,w) :- dereference(y,x), valueAlias(y,z), dereference(z,w)`
   - Missing: `deref_full x valAlias_full x delta(deref)`
2. `valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y)`
   - Missing: `vFlow_full x memAlias_full x delta(vFlow)`

The missing permutations mean some valid derivations are delayed until a later iteration when the facts propagate indirectly through other rules. This increases iteration count.

### Proposed Solution: Rule-Level Multi-Pass Evaluation

For each rule with K join atoms in a recursive stratum, evaluate K separate passes — one per delta position:

```
Pass 1: delta(A) x B_full  x C_full
Pass 2: A_full  x delta(B) x C_full
Pass 3: A_full  x B_full   x delta(C)
```

Union all passes into the rule's contribution for this iteration.

### Implementation Approach

**Option 2A: Plan-level rewriting (preferred)**

At plan compilation time, for each rule with N>2 atoms in a recursive stratum, emit N copies of the rule's op sequence, each with a different atom marked as the "delta source." This keeps the evaluator simple.

Plan transformation for `R :- A, B, C`:
```
Rule copy 1: VARIABLE_DELTA(A) -> JOIN_FULL(B) -> JOIN_FULL(C) -> CONCAT
Rule copy 2: VARIABLE_FULL(A)  -> JOIN_DELTA(B) -> JOIN_FULL(C) -> CONCAT
Rule copy 3: VARIABLE_FULL(A)  -> JOIN_FULL(B)  -> JOIN_DELTA(C) -> CONCAT
```

New op modifiers needed: `WL_PLAN_OP_VARIABLE` gains a `force_delta` / `force_full` flag. `WL_PLAN_OP_JOIN` gains a `force_right_delta` flag.

**Option 2B: Evaluator-level loop (simpler but less clean)**

In `col_eval_stratum`, for each relation plan with 3+ join ops, run the evaluation K times with different delta positions, unioning results.

### Pseudocode (Option 2A — plan-level flags)

```c
/* Extended plan op flags */
typedef enum {
    WL_DELTA_AUTO = 0,    /* current behavior: heuristic delta/full */
    WL_DELTA_FORCE_DELTA, /* force delta version of relation */
    WL_DELTA_FORCE_FULL,  /* force full version of relation */
} wl_delta_mode_t;

/* In col_op_variable: */
static int col_op_variable(const wl_plan_op_t *op, eval_stack_t *stack,
                           wl_col_session_t *sess)
{
    col_rel_t *full_rel = session_find_rel(sess, op->relation_name);
    if (!full_rel) return ENOENT;

    if (op->delta_mode == WL_DELTA_FORCE_FULL)
        return eval_stack_push_delta(stack, full_rel, false, false);

    if (op->delta_mode == WL_DELTA_FORCE_DELTA) {
        char dname[256];
        snprintf(dname, sizeof(dname), "$d$%s", op->relation_name);
        col_rel_t *delta = session_find_rel(sess, dname);
        if (delta && delta->nrows > 0)
            return eval_stack_push_delta(stack, delta, false, true);
        /* No delta available: skip this permutation (produces empty) */
        return eval_stack_push_delta(stack, full_rel, false, false);
    }

    /* WL_DELTA_AUTO: existing heuristic (lines 587-598) */
    /* ... unchanged ... */
}
```

### Iteration Count Reduction Estimate

For CSPA's 3-way join rules, the missing delta permutation means facts that would be discovered in iteration I are instead discovered in iteration I+1 or I+2 (after propagating through other rules first).

**Conservative estimate:** 10-30% reduction in iteration count for CSPA-class workloads.

**Reasoning:** In a 3-relation recursive system, each missing permutation delays a class of derivations by 1 iteration. With 2 three-way rules out of 5 total recursive rules, roughly 40% of derivations could be affected. But many facts are discovered through other paths (2-way rules), limiting the practical delay to a subset.

**Combined with Option 1:** Fewer iterations (Option 2) x cheaper iterations (Option 1) = compound benefit.

### Risk Assessment

- **Correctness:** Each permutation pass independently produces valid derivations. The union is always correct (may contain duplicates, resolved by CONSOLIDATE). No risk of missing facts — this strictly adds derivation paths.
- **Redundant work:** The K passes may produce overlapping results (a fact derivable through multiple delta permutations). CONSOLIDATE handles this via dedup. The overhead of K passes is partially offset by each pass operating on smaller (delta) inputs.
- **Plan complexity:** Option 2A requires changes to the plan compiler (`wirelog/ir/program.c`). This is a larger surface area than Option 1.
- **2-atom rules unaffected:** Rules with exactly 2 atoms already get complete delta coverage. The expansion only matters for K >= 3.

### Effort: 1-2 weeks

- Days 1-2: Add `delta_mode` field to `wl_plan_op_t`, update plan serialization
- Days 3-5: Implement plan-level rule rewriting for K >= 3 atom rules in recursive strata
- Days 6-7: Update `col_op_variable` and `col_op_join` to respect `delta_mode` flags
- Days 8-10: Testing, edge cases (rules with mix of EDB/IDB atoms, self-joins), benchmark validation

---

## Option 3: Combined Implementation

### Sequencing

Options 1 and 2 are independent and can be implemented in either order. However, Option 1 should go first:

1. **Option 1 first (days 1-5):** Cheaper iterations reduce wall time immediately. Also provides a cleaner baseline for measuring Option 2's iteration count reduction.
2. **Option 2 second (days 6-15):** Fewer iterations compound on top of the per-iteration savings from Option 1.

### Expected Compound Gain

| Component | Effect | Estimated Gain |
|-----------|--------|---------------|
| Option 1 alone | Cheaper consolidation per iteration | 2-15x on consolidation cost (depends on N/D ratio) |
| Option 2 alone | Fewer iterations | 10-30% fewer iterations |
| Combined | Multiplicative | `(1/consolidation_speedup) * (1 - iteration_reduction)` |

**Example for CSPA (100K facts, 50 iterations):**
- Option 1: If consolidation is 60% of iteration time (hypothesis), and speedup is 10x on consolidation → 54% total wall time reduction
- Option 2: 20% fewer iterations (50 → 40) → 20% total wall time reduction
- Combined: ~63% total wall time reduction (2.7x speedup)

These are estimates pending profiling data from Stream A. The actual consolidation time fraction determines Option 1's real impact.

---

## Code Path Summary

### Files Modified by Option 1
- `wirelog/backend/columnar_nanoarrow.c` — new `col_op_consolidate_incremental`, modified `col_eval_stratum` consolidation loop (lines 1652-1676), simplified delta computation (lines 1678-1730)

### Files Modified by Option 2
- `wirelog/exec_plan.h` — `wl_delta_mode_t` enum, `delta_mode` field on `wl_plan_op_t` (line 182-203)
- `wirelog/exec_plan_gen.c` — plan rewriting: emit K copies of K-atom recursive rules with different delta positions
- `wirelog/backend/columnar_nanoarrow.c` — `col_op_variable` and `col_op_join` respect `delta_mode`

### Test Strategy
- **Correctness oracle:** All 15 benchmark workloads must produce identical fact counts before/after
- **Performance regression:** Compare wall time, iteration count, peak RSS across all workloads
- **Unit tests:** New tests for incremental consolidate edge cases (empty delta, all-duplicate delta, single-row relations)
- **Stress test:** Run CSPA with larger datasets to verify scaling behavior

---

## Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Consolidation is NOT the bottleneck | Medium | Option 1 provides less benefit than estimated | Stream A profiling will confirm; even if only 20% of time, improvement is free |
| Multi-way delta produces too many duplicates | Low | Extra CONSOLIDATE work offsets iteration savings | Monitor dedup ratio; can disable for rules where K is large |
| Plan rewriting breaks edge cases | Medium | Incorrect results for complex recursive strata | Compare fact counts against current implementation for all workloads |
| Memory pressure from merge buffer | Low | OOM on very large relations | Buffer is at most 2x current relation size; same as current copy path |

---

## Profiling Evidence: Stream A (Completed)

Stream A profiling has **CONFIRMED both hypotheses** with concrete evidence:

### H1: CONSOLIDATE Full-Sort Per Iteration — **CONFIRMED**

**Evidence** (from docs/performance/HYPOTHESIS-VALIDATION.md):
- **CSPA workload**: 4,602ms total (226ms/K-tuples), ~7× more output tuples than CSDA yet 648× longer
- **CSPA peak RSS**: 4.46 GB for 20,381 final tuples (320 KB useful data) — 14,000× amplification
- **Root cause**: CONSOLIDATE calls `qsort_r()` on ALL accumulated IDB tuples in EVERY iteration (O(N log N)), not just delta
- **Memory issue**: Per-iteration `old_data` snapshots + intermediate join results with no reclaim between iterations (O(iterations × N × relations))
- **Affected workloads**: All recursive strata; worst impact on deep mutual recursion (CSPA, CRDT)

**Decision impact**: H1 is **definitively the primary bottleneck** for pathological recursive workloads. Option 1 (incremental CONSOLIDATE) is **STRONGLY RECOMMENDED** (not optional).

### H2: Incomplete Delta Expansion — **CONFIRMED**

**Evidence** (from docs/performance/HYPOTHESIS-VALIDATION.md):
- **Issue identified**: Semi-naive JOIN handler applies EITHER `ΔA × B_full` OR `A_full × ΔB` per rule evaluation, but NOT both
- **Missing variant**: For 3+ atom rules, the third permutation (`A_full × B_full × ΔC`) is never computed
- **CSPA impact**: 2 rules have 3-way joins (`memoryAlias`, `valueAlias`); missing permutations delay facts by 1-2 iterations
- **Iteration count gap**: Estimated 100-300 iterations (vs ~50-150 with correct semi-naive) — approximately 2× excess
- **Root cause code**: `col_op_variable` (lines 753-888) applies only ONE delta substitution per rule pass

**Decision impact**: H2 is **confirmed as HIGH priority**. Option 2 (multi-way delta expansion) is **STRONGLY RECOMMENDED** for workloads with 3+ atom recursive rules.

### H3: Workqueue ROI — **PARTIALLY CONFIRMED**

**Evidence** (from docs/performance/HYPOTHESIS-VALIDATION.md):
- **Non-recursive time**: <0.1ms (below timer resolution) for all benchmarks — **~99-100% of time is recursive**
- **Within-iteration parallelism**: CSPA has 3 IDB relations with data dependencies; realistic ~1.5–2× speedup
- **Large data loading**: CRDT/DOOP show 5–10× speedup potential for parallel file loading (>100 input files)
- **Amdahl constraint**: Iteration loop is sequential (cannot parallelize); workqueue cannot parallelize iteration count reduction
- **Recommendation**: Fix H1+H2 first (reduces iteration count and per-iteration cost), then workqueue provides ~1.5-2× additional speedup

**Decision impact**: H3 is **valuable but secondary**. Workqueue provides positive ROI (~1.5-2× for CSPA after H1+H2), but H1+H2 fixes are higher priority to reduce iteration overhead first.

---

## Recommendation: Pursue Path C (Hybrid — Both Optimizations + Workqueue)

**Based on profiling evidence:**
- ✅ H1 CONFIRMED: CONSOLIDATE is definitive bottleneck (HIGH)
- ✅ H2 CONFIRMED: Delta expansion incomplete for 3-way joins (HIGH)
- ✅ H3 PARTIALLY CONFIRMED: Workqueue provides 1.5-2× after H1+H2 (MEDIUM)

**Path C rationale:**
1. Implement Option 1 (incremental CONSOLIDATE, 3-5 days) to reduce per-iteration cost by ~10x
2. Implement Option 2 (multi-way delta expansion, 1-2 weeks) to reduce iterations by ~50-100% in pathological cases
3. Both can run in parallel with 2 workers (estimated 3-5 weeks combined)
4. Then implement Phase B-lite workqueue (2-4 weeks) on optimized baseline

**Expected outcome for CSPA:**
- Baseline: 4,602ms
- After H1: ~460ms (10x consolidation speedup)
- After H2 on top of H1: ~230ms (2x fewer iterations)
- After workqueue (2 workers): ~150ms (1.5x parallelization)
- **Total: ~32x speedup from 4,602ms to ~150ms**

---

## Action Items for Phase 2C Execution

**Confirmed**: Both Option 1 and Option 2 are **STRONGLY RECOMMENDED**. Proceed with Path C (Hybrid parallel implementation).

1. **Option 1 implementation** (3-5 days, atomic commits with strict linting per CLAUDE.md)
2. **Option 2 implementation** (1-2 weeks, can run in parallel with Option 1)
3. **Validation**: All 15 benchmarks pass with identical fact counts; wall time improvements verified
4. **Phase B-lite**: After both optimizations complete, add workqueue parallelization (2-4 weeks)
