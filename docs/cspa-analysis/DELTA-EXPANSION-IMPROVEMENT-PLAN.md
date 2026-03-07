# Delta Expansion Improvement Plan

**Date:** 2026-03-07
**Status:** Ready for Implementation
**Scope:** Semi-naive delta expansion completeness for k=2 rules

---

## 1. Problem Statement

The semi-naive evaluator produces ~2x excess iterations for CSPA because k=2
recursive rules are not expanded into both required delta permutations.

**Root cause:** `exec_plan_gen.c:1087` applies multi-way delta expansion only
when `k >= 3`. Rules with exactly 2 IDB body atoms rely on the `WL_DELTA_AUTO`
heuristic, which covers only 1 of 2 required permutations per iteration.

---

## 2. Affected Rules

CSPA has 5 recursive rules in a single mutually-recursive stratum. Two critical
rules have k=2 IDB body atoms and are NOT expanded:

| Rule | Source | K (IDB) | Expanded? | Impact |
|------|--------|---------|-----------|--------|
| `valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y)` | `cspa.dl:13` | 2 | **NO** | Transitive closure core |
| `valueAlias(x,y) :- valueFlow(z,x), valueFlow(z,y)` | `cspa.dl:16` | 2 | **NO** | Alias derivation core |
| `valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y)` | `cspa.dl:17` | 3 | YES | Already expanded |
| `valueFlow(x,y) :- assign(x,z), memoryAlias(z,y)` | `cspa.dl:14` | 1 | N/A | Single IDB atom |
| `memoryAlias(x,w) :- deref(y,x), valueAlias(y,z), deref(z,w)` | `cspa.dl:15` | 1 | N/A | Single IDB atom |

The two k=2 rules are self-joins on IDB relations and form the computational
core of the analysis. Missing one delta permutation delays fact discovery by
1+ iterations per derivation chain.

---

## 3. How AUTO Fails for k=2

Tracing `valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y)`:

1. `col_op_variable` (`columnar_nanoarrow.c:816-821`):
   AUTO picks `delta(valueFlow)` when `delta->nrows < full->nrows`
   Sets `is_delta = true` on the stack entry.

2. `col_op_join` (`columnar_nanoarrow.c:993`):
   AUTO checks `!left_e.is_delta` -- since left IS delta, right-delta is **skipped**.
   Uses full right relation.

3. **Result:** Only `delta(VF) x VF_full` is computed.
   The permutation `VF_full x delta(VF)` is **never computed**.

Every derivation requiring a new right-hand fact waits until the next iteration
when that fact propagates through the left position. This doubles the iteration
count for transitive closure-like computations.

---

## 4. Plan Rewriting Design

### 4.1 How K Copies Are Generated

The existing `expand_multiway_delta()` at `exec_plan_gen.c:981-1052` already
handles arbitrary K values. For k=2, it produces:

```
Copy 0: [ops with delta_pos[0]=FORCE_DELTA, delta_pos[1]=FORCE_FULL] + CONCAT
Copy 1: [ops with delta_pos[0]=FORCE_FULL, delta_pos[1]=FORCE_DELTA] + CONCAT
CONSOLIDATE
```

Total ops = 2 * original_op_count + 2 (CONCATs) + 1 (CONSOLIDATE).

### 4.2 How the Evaluator Distinguishes Delta Positions

Each plan copy has explicit `delta_mode` annotations on its operators:

**Copy 0** (delta at position 0):
```
VARIABLE(valueFlow, delta_mode=FORCE_DELTA)  -> pushes delta(VF)
JOIN(valueFlow,     delta_mode=FORCE_FULL)   -> joins with VF_full
MAP/FILTER...                                -> delta_mode=AUTO (irrelevant)
```

**Copy 1** (delta at position 1):
```
VARIABLE(valueFlow, delta_mode=FORCE_FULL)   -> pushes VF_full
JOIN(valueFlow,     delta_mode=FORCE_DELTA)  -> joins with delta(VF)
MAP/FILTER...                                -> delta_mode=AUTO
```

The evaluator at `col_op_variable` (`columnar_nanoarrow.c:804-813`) and
`col_op_join` (`columnar_nanoarrow.c:984-1002`) already respect FORCE_DELTA
and FORCE_FULL. No evaluator changes needed.

### 4.3 Merging Results from K Copies

The K copies are followed by K CONCAT ops and 1 CONSOLIDATE op:

```
[Copy 0 result on stack]
[Copy 1 result on stack]
CONCAT  <- pops 2, pushes union
CONSOLIDATE <- pops 1, pushes sorted+deduped result
```

The CONCAT operator unions the results. The CONSOLIDATE operator deduplicates.
This is identical to how k>=3 expansion already works.

### 4.4 No Materialization Needed for k=2

The materialization hint logic at `exec_plan_gen.c:1021-1033` marks JOINs as
materializable when `join_idx < k - 2`. For k=2, this means `join_idx < 0`,
so **no JOINs are marked materialized**. This is correct: with only 2 copies
and 1 join each, there's no shared prefix to cache.

---

## 5. Execution Model

### 5.1 Current (k=2, AUTO heuristic)

```
Per iteration:
  1. Evaluate rule plan once
  2. AUTO picks ONE permutation: delta(A) x B_full
  3. Missing: A_full x delta(B)
  4. Facts from missing permutation delayed to next iteration
```

### 5.2 Proposed (k=2, expanded)

```
Per iteration:
  1. Evaluate expanded plan (2 copies + concat + consolidate)
  2. Copy 0: delta(A) x B_full  (FORCE_DELTA left, FORCE_FULL right)
  3. Copy 1: A_full x delta(B)  (FORCE_FULL left, FORCE_DELTA right)
  4. CONCAT: union both results
  5. CONSOLIDATE: dedup
  6. All permutations covered -- no delayed facts
```

### 5.3 First Iteration (No Delta Available)

On iteration 0, no delta relations exist. The FORCE_DELTA path in
`col_op_variable` (`columnar_nanoarrow.c:807-813`) falls back to the full
relation when no delta is found:

```c
if (op->delta_mode == WL_DELTA_FORCE_DELTA) {
    if (delta && delta->nrows > 0) {
        return eval_stack_push_delta(stack, delta, false, true);
    }
    /* No delta available: fall back to full relation */
    return eval_stack_push_delta(stack, full_rel, false, false);
}
```

Both copies effectively compute `full x full` on the first iteration, producing
identical results. The CONSOLIDATE deduplicates. This is correct but produces
2x work on iteration 0 only. Subsequent iterations benefit from proper delta
splitting.

---

## 6. Architectural Assessment

**This is an incremental improvement, not a fundamental change.**

The fix is a one-line threshold change. All supporting infrastructure already
exists:

- `wl_delta_mode_t` enum: `exec_plan.h:153-157`
- `expand_multiway_delta()`: `exec_plan_gen.c:981-1052`
- `count_delta_positions()`: `exec_plan_gen.c:840-871`
- `clone_plan_op()`: `exec_plan_gen.c:877-961`
- Evaluator FORCE_DELTA/FORCE_FULL handling: `columnar_nanoarrow.c:804-821, 984-1003`

No new data structures, no evaluator changes, no API changes.

---

## 7. Backward Compatibility

### 7.1 Impact on Existing Workloads

| Workload | k=2 IDB Rules | Impact |
|----------|---------------|--------|
| TC | `path :- path, edge` (k=1: edge is EDB) | None -- k=1 |
| Reach | `reach :- reach, edge` (k=1) | None |
| CC | `cc :- cc, edge` (k=1) | None |
| SSSP | `dist :- dist, edge` (k=1, aggregation) | None |
| Andersen | `pt :- pt, assign` (k=1) | None |
| Polonius | Various (k=1 or k=2) | May benefit |
| CSPA | `valueFlow`, `valueAlias` (k=2) | **Primary beneficiary** |
| DOOP | Various (k=2 through k=9) | k=2 rules benefit; k>=3 already expanded |
| CRDT | Large data, simple rules | Minimal impact |

k=2 expansion produces strictly more facts per iteration (superset of current
output). Final results are identical -- only convergence speed changes.

### 7.2 Correctness Guarantee

Semi-naive theory guarantees: the union of all K delta permutations is a
superset of any single permutation. Extra intermediate duplicates are eliminated
by CONSOLIDATE. The fixed-point converges to the same unique result.

**Formal argument:** Let R* be the minimal fixed-point. With incomplete delta
expansion, each iteration computes a subset S_i of the new facts. With complete
expansion, each iteration computes a superset S'_i where S_i is a subset of S'_i.
Both converge to R*, but complete expansion converges in fewer iterations.

---

## 8. Implementation Roadmap

| Step | Change | Effort | Dependencies |
|------|--------|--------|--------------|
| 1 | Add iteration counter logging to `col_eval_stratum` (baseline) | 30 min | None |
| 2 | Change `k >= 3` to `k >= 2` at `exec_plan_gen.c:1087` | 5 min | Step 1 |
| 3 | Run CSPA benchmark, measure iteration count reduction | 15 min | Step 2 |
| 4 | Run TC, Reach, CC -- verify simple workloads unaffected | 15 min | Step 2 |
| 5 | Run full 15-workload regression suite | 1 hr | Step 3-4 pass |
| 6 | (Optional) Optimize first-iteration 2x overhead: skip FORCE_DELTA copy when no delta exists | 1 hr | Step 5 |
| 7 | (Future) Runtime delta expansion for k>=3 per OPTION2-DESIGN.md | 1-2 weeks | Independent |

**Total effort for core fix: ~2 hours including validation.**

---

## 9. Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Performance regression from 2x plan size | Very Low | k=2 produces only 2 copies (vs 8 for DOOP k=8 that caused regression) |
| First-iteration double work | Low | One-time cost; subsequent iterations benefit. Optional step 6 eliminates this. |
| Correctness issue | Near Zero | Same infrastructure proven correct for k>=3. Semi-naive theory guarantees. |
| Memory increase from larger plans | Negligible | 2 copies of small op arrays (~hundreds of bytes) |

---

## 10. Success Criteria

- **Primary:** CSPA iteration count drops from ~100-300 to ~50-150 (per OPTIMIZATION-STRATEGY.md:413 estimate of "2x excess iterations")
- **Secondary:** CSPA wall time improves by 15-40% (fewer iterations, each same cost)
- **Regression gate:** All 15 workloads produce identical output tuples
- **Combined with incremental CONSOLIDATE:** Expected compound speedup of 2-4x on CSPA

---

## 11. Pseudocode Summary

The change is a single threshold adjustment:

```c
// exec_plan_gen.c:1087
// BEFORE:
if (k >= 3) {
    uint32_t new_count = 0;
    wl_plan_op_t *new_ops = expand_multiway_delta(
        rel->ops, rel->op_count, dpos, k, &new_count);
    ...
}

// AFTER:
if (k >= 2) {  // <-- only change
    uint32_t new_count = 0;
    wl_plan_op_t *new_ops = expand_multiway_delta(
        rel->ops, rel->op_count, dpos, k, &new_count);
    ...
}
```

All downstream machinery (`expand_multiway_delta`, evaluator delta_mode handling,
CONCAT, CONSOLIDATE) already works correctly for any k >= 2.

---

## 12. References

- `wirelog/exec_plan_gen.c:1087` -- k>=3 threshold (root cause)
- `wirelog/exec_plan_gen.c:981-1052` -- `expand_multiway_delta` (handles any k)
- `wirelog/exec_plan_gen.c:840-871` -- `count_delta_positions`
- `wirelog/backend/columnar_nanoarrow.c:804-821` -- `col_op_variable` delta_mode
- `wirelog/backend/columnar_nanoarrow.c:976-1003` -- `col_op_join` delta_mode
- `wirelog/backend/columnar_nanoarrow.c:1864` -- fixed-point iteration loop
- `wirelog/exec_plan.h:130-157` -- `wl_delta_mode_t` enum
- `wirelog/exec_plan.h:237` -- `wl_plan_op_t.delta_mode` field
- `bench/workloads/cspa.dl:13,16` -- k=2 self-join rules
- `docs/performance/OPTION2-DESIGN.md` -- comprehensive CSE design for k>=3
- `docs/performance/OPTIMIZATION-STRATEGY.md:412-413` -- 2x excess iteration estimate
