# CSPA Delta Expansion Analysis

**Date**: 2026-03-07
**Analyst**: Task #3 — Deep analysis of delta expansion bottleneck
**Scope**: CSPA workload, semi-naive evaluator, `exec_plan_gen.c` + `columnar_nanoarrow.c`

---

## 1. CSPA Rule Structure

CSPA (Context-Sensitive Points-to Analysis) defines 3 mutually-recursive IDB relations
(`valueFlow`, `memoryAlias`, `valueAlias`) in a single recursive stratum:

```prolog
-- Base rules (EDB → IDB, evaluated once in stratum)
valueFlow(y, x)  :- assign(y, x).
valueFlow(x, x)  :- assign(x, _).
valueFlow(x, x)  :- assign(_, x).
memoryAlias(x,x) :- assign(_, x).
memoryAlias(x,x) :- assign(x, _).

-- Recursive rules (all in the same stratum's fixed-point loop)
R1: valueFlow(x, y)   :- valueFlow(x, z), valueFlow(z, y).            -- K=2 IDB atoms
R2: valueFlow(x, y)   :- assign(x, z), memoryAlias(z, y).             -- K=1 IDB atom
R3: memoryAlias(x, w) :- dereference(y, x), valueAlias(y, z),
                          dereference(z, w).                            -- K=1 IDB atom
R4: valueAlias(x, y)  :- valueFlow(z, x), valueFlow(z, y).            -- K=2 IDB atoms
R5: valueAlias(x, y)  :- valueFlow(z, x), memoryAlias(z, w),
                          valueFlow(w, y).                              -- K=3 IDB atoms
```

**IDB atom count K** = number of IDB relations in the rule body (relations whose `$d$name`
delta form exists in the same stratum's fixed-point). For CSPA, IDB = {valueFlow, memoryAlias, valueAlias}.

Source: `bench/workloads/cspa.dl`, `bench/bench_flowlog.c:594-616`

---

## 2. Current Delta Permutation Coverage

### 2.1 The `k >= 3` threshold

Multi-way delta expansion is implemented in `exec_plan_gen.c:1061-1103` (`rewrite_multiway_delta`).
The trigger condition is:

```c
// exec_plan_gen.c:1087
if (k >= 3) {
    uint32_t new_count = 0;
    wl_plan_op_t *new_ops = expand_multiway_delta(
        rel->ops, rel->op_count, dpos, k, &new_count);
    ...
}
```

**Rules with K < 3 are NOT expanded.** They fall through to the original `WL_DELTA_AUTO` heuristic.

### 2.2 K=3 rule: COMPLETE expansion (R5)

For `valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y)` (K=3):

`expand_multiway_delta` (lines 980-1052) generates **3 copies** of the rule plan:

| Copy | Permutation | delta_mode assignments |
|------|-------------|------------------------|
| d=0  | **δvalueFlow(z,x)** × memoryAlias(z,w) × valueFlow(w,y) | FORCE_DELTA on pos 0; FORCE_FULL on pos 1,2 |
| d=1  | valueFlow(z,x) × **δmemoryAlias(z,w)** × valueFlow(w,y) | FORCE_DELTA on pos 1; FORCE_FULL on pos 0,2 |
| d=2  | valueFlow(z,x) × memoryAlias(z,w) × **δvalueFlow(w,y)** | FORCE_DELTA on pos 2; FORCE_FULL on pos 0,1 |

All 3 of K=3 permutations are evaluated. **R5 is correctly handled.**

CSE materialization hint: the first K-2=1 JOIN (memoryAlias position) is marked `materialized=true`
to allow the evaluator to cache and reuse the `valueFlow × memoryAlias` prefix across copies.

### 2.3 K=2 rules: INCOMPLETE expansion (R1, R4)

For K=2 rules, the `k >= 3` guard prevents multi-way expansion. They use `WL_DELTA_AUTO`.

**R1: `valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y)`** (self-join TC)

Plan ops: `VARIABLE(valueFlow)` → `JOIN(right: valueFlow)`

Runtime behavior (from `columnar_nanoarrow.c:793-818` and `971-998`):

```c
// VARIABLE op (AUTO): uses $d$valueFlow when delta.nrows < full.nrows
bool use_delta = (delta && delta->nrows > 0 && delta->nrows < full_rel->nrows);
// → left.is_delta = true

// JOIN op (AUTO): only substitutes right-delta when left is NOT already delta
} else if (op->delta_mode != WL_DELTA_FORCE_FULL && !left_e.is_delta && ...) {
//  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//  Since left.is_delta == true, this branch is NEVER taken
//  → right stays FULL in all cases
```

Per-iteration result:
- **Evaluated**: `δvalueFlow(x,z)` × `valueFlow(z,y)` (left=delta, right=full)
- **Never evaluated**: `valueFlow(x,z)` × `δvalueFlow(z,y)` (left=full, right=delta)

**R4: `valueAlias(x,y) :- valueFlow(z,x), valueFlow(z,y)`** (K=2, cross-join)

Same pattern: `VARIABLE(valueFlow)` → `JOIN(right: valueFlow)`. Same AUTO logic applies.

Per-iteration result:
- **Evaluated**: `δvalueFlow(z,x)` × `valueFlow(z,y)`
- **Never evaluated**: `valueFlow(z,x)` × `δvalueFlow(z,y)`

### 2.4 Summary of permutation coverage

| Rule | K | Permutations Generated | Permutations Missing | Status |
|------|---|------------------------|----------------------|--------|
| R1: `valueFlow :- valueFlow, valueFlow` | 2 | 1 of 2 (δvF×vF) | 1 of 2 (vF×δvF) | **INCOMPLETE** |
| R2: `valueFlow :- assign, memoryAlias` | 1 | 1 of 1 | none | OK |
| R3: `memoryAlias :- deref, valueAlias, deref` | 1 | 1 of 1 | none | OK |
| R4: `valueAlias :- valueFlow, valueFlow` | 2 | 1 of 2 (δvF×vF) | 1 of 2 (vF×δvF) | **INCOMPLETE** |
| R5: `valueAlias :- valueFlow, memoryAlias, valueFlow` | 3 | 3 of 3 (all) | none | **COMPLETE** |

---

## 3. Execution Trace: What Fires Per Iteration

The semi-naive fixed-point loop (`col_eval_stratum`, lines 1864-2083) runs all 3 IDB
relations per iteration. Delta relations `$d$valueFlow`, `$d$memoryAlias`, `$d$valueAlias`
from the previous iteration are registered at iteration start (lines 1865-1878).

For each iteration N (after the base case):

```
Iteration N:
  Evaluate valueFlow:
    R1: δvalueFlow(x,z) × valueFlow(z,y)    [fires]
        valueFlow(x,z)  × δvalueFlow(z,y)   [MISSING — would catch old-source + new-bridge]
    R2: assign(x,z)     × δmemoryAlias(z,y) [fires, K=1, AUTO picks δ on right since left is EDB]

  Evaluate valueAlias:
    R4: δvalueFlow(z,x) × valueFlow(z,y)    [fires]
        valueFlow(z,x)  × δvalueFlow(z,y)   [MISSING]
    R5: δvalueFlow(z,x) × memoryAlias(z,w)  × valueFlow(w,y)   [fires — copy d=0]
        valueFlow(z,x)  × δmemoryAlias(z,w) × valueFlow(w,y)   [fires — copy d=1]
        valueFlow(z,x)  × memoryAlias(z,w)  × δvalueFlow(w,y)  [fires — copy d=2]

  Evaluate memoryAlias:
    R3: deref(y,x) × δvalueAlias(y,z) × deref(z,w) [fires, K=1]
```

The **missing permutations for R1 and R4** correspond to the case where:
- An OLD source path `(x,z)` now connects to a NEW bridge path `(z,y)`.
- The new bridge `δvalueFlow(z,y)` was derived in the current or previous iteration.
- Without the missing permutation, this connection is not discovered in the same iteration;
  it requires a subsequent iteration where `(x,y)` is in the delta and the recursive join fires again.

---

## 4. Iteration Reduction Estimate

### 4.1 Theoretical analysis for R1 (TC self-recursion)

Standard semi-naive for `R(x,y) :- R(x,z), R(z,y)` with both permutations:

```
ΔR^(n+1) = (ΔR^n ⊳⊲ R^n) ∪ (R^(n+1) ⊳⊲ ΔR^n)
```

With only the left permutation (current behavior):
```
ΔR^(n+1) ⊆ ΔR^n ⊳⊲ R^n    (only new-left × old-right)
```

The missing permutation `R^new ⊳⊲ ΔR^n` captures:
- Facts derivable via old sources + new bridges
- In CSPA: new `valueFlow(z,y)` tuples that connect to existing `valueFlow(x,z)` paths

For a linear graph A→B→C→D→E (diameter 4), both permutations together behave
asymmetrically — each extends paths from EITHER end, halving effective diameter in many cases.

### 4.2 CSPA-specific cross-dependency effect

The mutual recursion creates a 3-step dependency cycle:
```
new valueFlow → new valueAlias (R4, R5)
             → new memoryAlias (R3)
             → new valueFlow   (R2)
             → new valueFlow   (R1 TC)
```

The cycle length is ~3 iterations minimum. Missing permutations in R1 and R4 mean:
- Some `valueFlow` derivations that could complete in iteration N instead complete in N+1
- Each extra iteration forces one more full CONSOLIDATE pass over all 3 relations

### 4.3 Quantitative estimate

From `CSPA-VALIDATION-RESULTS.md` (line 124) and `benchmark-baseline-2026-03-07.md`:

| Metric | Baseline | Estimated with K=2 fix |
|--------|----------|------------------------|
| Estimated iterations | ~100–300 | ~65–220 (20–35% fewer) |
| Median wall time | 4,602 ms | Proportional reduction |
| CONSOLIDATE passes | ~100–300 × 3 relations | ~65–220 × 3 relations |

The 20–35% iteration reduction estimate is consistent with hypothesis H2 target (">20% fewer iterations").
Iteration reduction does NOT translate 1:1 to wall-time reduction because:
- Late iterations dominate cost (larger N → larger CONSOLIDATE sort)
- But fewer iterations = fewer of the expensive late-stage sorts

If the last 30% of iterations account for 60% of runtime (due to O(N log N) sort growth),
a 25% iteration reduction could yield ~15% wall-time improvement from this fix alone.

---

## 5. Architectural Assessment

### 5.1 Fix: Lower threshold from `k >= 3` to `k >= 2`

**Location**: `exec_plan_gen.c:1087`

**Change**:
```c
// Before:
if (k >= 3) {

// After:
if (k >= 2) {
```

**What this generates for K=2 rules** (via existing `expand_multiway_delta`):
- Copy d=0: FORCE_DELTA on pos 0, FORCE_FULL on pos 1 → `δA × B`
- Copy d=1: FORCE_FULL on pos 0, FORCE_DELTA on pos 1 → `A × δB`
- CONCAT (boundary marker)
- CONCAT (boundary marker)
- CONSOLIDATE (intra-rule dedup before merging with relation)

Total new ops per K=2 rule: `2 × op_count + 3` (vs current `op_count`).

**CSE materialization for K=2**: The hint logic `if (join_idx < k - 2)` evaluates to
`join_idx < 0` for K=2, so **no JOINs are marked materialized**. This is correct —
there is no shared prefix to cache for 2 copies with only 1 JOIN each.

### 5.2 Compatibility with existing infrastructure

| Component | Impact |
|-----------|--------|
| `expand_multiway_delta` | No change; already handles K=2 correctly |
| `col_op_variable` / `col_op_join` | No change; FORCE_DELTA/FORCE_FULL already implemented |
| `col_eval_relation_plan` | No change; CONCAT/CONSOLIDATE ops already handled |
| `col_mat_cache` (CSE materialization) | No change; K=2 generates no materialization hints |
| Correctness oracle: 20,381 tuples | Must be validated after fix |

### 5.3 Risk assessment

**Low risk**. The multi-way expansion infrastructure was designed to be general over K.
The existing K=3 case exercises every code path that K=2 would use:
- `expand_multiway_delta` loop with `d=0..k-1`
- FORCE_DELTA / FORCE_FULL delta_mode dispatch in VARIABLE and JOIN ops
- CONCAT boundary handling in `col_eval_relation_plan`

The only new behavior is: K=2 rules now produce 2 copies instead of using AUTO heuristic.
The correctness requirement (output = 20,381 tuples) must be verified after the change.

**Potential overhead**: Each K=2 rule evaluation doubles the plan ops and adds 2 CONCATs +
1 CONSOLIDATE. For CSPA rules R1 and R4, this roughly doubles evaluation work per rule per
iteration. However, the iteration count reduction (20–35%) should more than compensate,
since the dominant cost is the stratum-level CONSOLIDATE (H1), not individual rule evaluation.

### 5.4 Does this require major redesign?

**No.** This is a 1-line change to the threshold in `exec_plan_gen.c`. The plan generator,
evaluator, and CSE infrastructure all already support the K=2 expanded form. No new data
structures, algorithms, or backend changes are required.

The change is orthogonal to the CONSOLIDATE optimization (H1 / incremental sort fix) and
can be applied independently or in combination with it.

---

## 6. Summary

| Question | Answer |
|----------|--------|
| Which rule has K=3 IDB atoms? | R5: `valueAlias :- valueFlow, memoryAlias, valueFlow` |
| Is R5 fully expanded? | **Yes** — all 3 permutations (δvF×mA×vF, vF×δmA×vF, vF×mA×δvF) |
| Which rules are K=2? | R1: `valueFlow :- valueFlow, valueFlow`; R4: `valueAlias :- valueFlow, valueFlow` |
| Are R1, R4 expanded? | **No** — K=2 falls below the `k >= 3` threshold |
| What fires for R1, R4? | Only `δA × B`; the permutation `A × δB` never fires |
| Root cause | `exec_plan_gen.c:1087`: `if (k >= 3)` should be `if (k >= 2)` |
| Estimated iteration reduction | 20–35% fewer iterations |
| Estimated wall-time reduction | ~15% (compounded with H1 fix: potentially 30–50%) |
| Implementation complexity | **Minimal** — 1-line change, no redesign needed |
| Risk | **Low** — existing infrastructure handles K=2; correctness validation required |

---

## References

- `exec_plan_gen.c:829-1103` — `rewrite_multiway_delta`, `expand_multiway_delta`, `count_delta_positions`
- `wirelog/backend/columnar_nanoarrow.c:789-818` — VARIABLE op delta_mode dispatch
- `wirelog/backend/columnar_nanoarrow.c:971-998` — JOIN op AUTO heuristic (`!left_e.is_delta` guard)
- `wirelog/backend/columnar_nanoarrow.c:1864-2083` — Semi-naive fixed-point iteration loop
- `bench/workloads/cspa.dl` — CSPA rule definitions
- `bench/bench_flowlog.c:594-616` — CSPA inline template
- `docs/performance/benchmark-baseline-2026-03-07.md:78-133` — CSPA deep-profile analysis
- `docs/performance/CSPA-VALIDATION-RESULTS.md` — H2 hypothesis target (>20% iteration reduction)
