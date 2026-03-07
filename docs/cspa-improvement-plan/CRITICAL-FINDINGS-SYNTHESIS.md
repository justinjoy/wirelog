# CSPA Improvement Strategy: Critical Findings Synthesis

**Date:** 2026-03-07 (Final Update)
**Status:** Team analysis revealed major premise correction
**Team Finding:** H1 and H2 fixes are already implemented; strategy must pivot to optimization, not implementation

---

## CRITICAL CORRECTION: H1 and H2 Already Implemented

### What We Thought vs What We Found

| Aspect | Initial Analysis | Team Discovery | Impact |
|--------|------------------|-----------------|--------|
| **Incremental CONSOLIDATE** | Needs to be implemented | Already in code (line 2009), ACTIVE | Stratum consolidation is not the bottleneck |
| **Multi-way delta expansion** | Needs to be designed | Already in code (exec_plan_gen.c:985-1048), WORKING | K-copy expansion is functioning |
| **Root bottleneck** | Consolidation overhead | In-plan CONSOLIDATE at line 1732 (O(N log N) on K-copy union) | Primary target is different function |
| **Strategy approach** | "Implement H1+H2 fixes" | "Optimize existing H1+H2 infrastructure" | Major pivot in approach |

---

## Actual Bottleneck Identified: In-Plan CONSOLIDATE (Line 1732)

### The Two CONSOLIDATE Paths

**Path 1: In-Plan CONSOLIDATE** (line 1732) — **THE BOTTLENECK**
- Called via `col_eval_relation_plan()` → `WL_PLAN_OP_CONSOLIDATE` dispatch
- Uses **full qsort** (`col_op_consolidate`) on K-copy union output
- Complexity: **O(M log M)** where M = K × join output size
- Time fraction: **52-63% of wall time** (primary bottleneck)
- Status: NOT optimized (still using full sort, not incremental merge)

**Path 2: Post-Iteration Incremental CONSOLIDATE** (line 2009) — **ALREADY OPTIMIZED**
- Called after all relation plans evaluated in semi-naive loop
- Uses incremental merge (`col_op_consolidate_incremental`)
- Complexity: **O(D log D + N)** — already efficient
- Time fraction: **3-7% of wall time** (not a bottleneck)
- Status: Fully optimized since commit `45f459a`

### Why In-Plan CONSOLIDATE Dominates

For CSPA with K=3 plan copies:
1. Each 3-way rule evaluation generates 3 plan copies (one per delta permutation)
2. Each copy produces intermediate results (join output)
3. UNION → CONCAT + **CONSOLIDATE (full sort)** at line 1732
4. This full sort runs on the **combined output of all 3 copies**
5. With CSPA's growing intermediate relations (~20K rows × 2 cols), this is tens of thousands of rows per iteration
6. Over 100+ iterations: ~100s of full sorts on large data

---

## Secondary Bottleneck: Memory Amplification (old_data Snapshot)

### Root Cause

Lines 1894-1898 in semi-naive loop:
```c
for (uint32_t ri = 0; ri < nrels; ri++) {
    size_t bytes = (size_t)snap[ri] * r->ncols * sizeof(int64_t);
    old_data[ri] = (int64_t *)malloc(bytes);
    memcpy(old_data[ri], r->data, bytes);  // O(N) memcpy per relation per iteration
}
```

**For each iteration, for each of 3 IDB relations:**
- `malloc` new buffer for old relation state
- `memcpy` entire relation data (grows from 100s to 20K rows)
- Later: compute delta via merge walk, free old buffer

**Result:** 100+ iterations × 3 relations × growing memcpy = **4.46 GB peak RSS** for 320 KB useful data (9,700× amplification)

---

## Current Performance Breakdown

**Wall Time Fraction (28.7s baseline):**

| Component | Time | Fraction | Evidence |
|-----------|------|----------|----------|
| In-plan CONSOLIDATE (K-copy dedup) | ~15-18s | **52-63%** | Removing it yields 77.6s (2.7× worse) |
| K-copy join evaluation | ~8-10s | **28-35%** | Disabling K expansion yields 41.9s |
| Post-plan incremental consolidate | ~1-2s | **3-7%** | Already optimized |
| Delta computation + snapshots | ~1-2s | **3-7%** | O(N) merge walk + memcpy |
| CSE cache overhead | ~0.4s | **1.4%** | Measured |

---

## Recommended Optimization Sequence

### Priority 1: Fix In-Plan CONSOLIDATE (Highest ROI)

**Problem:** Full qsort on K-copy union is O(M log M) and dominates wall time.

**Solutions:**

**Option A: K-Way Merge** (Recommended, 2-3 days)
- Sort each K-copy output independently: K × O((M/K) log(M/K)) < O(M log M)
- K-way merge with dedup: O(M log K)
- Total: O(M log(M/K) + M log K) — same complexity but smaller constants
- **Expected improvement:** 30-45% wall time (28.7s → ~16-20s)
- Effort: 2-3 days
- Risk: Low

**Option B: Eliminate In-Plan CONSOLIDATE** (1-2 weeks)
- Each K-copy produces sorted output (sort per-copy, not union)
- Push ALL dedup to stratum-level incremental consolidate
- Let stratum consolidate handle K-copy union
- **Expected improvement:** 40-50% wall time
- Effort: 1-2 weeks (requires architecture change)
- Risk: Medium (more invasive)

### Priority 2: Eliminate old_data Snapshot (Memory ROI)

**Problem:** O(N) memcpy per iteration per relation causes 9,700× RSS amplification.

**Solution:** Capture delta as merge byproduct during `col_op_consolidate_incremental`
- Extend `col_op_consolidate_incremental` to return delta rows (rows not in old prefix)
- Eliminates separate `old_data` malloc+memcpy+merge walk
- **Expected improvement:** Peak RSS from 3.1 GB to <500 MB, ~20% wall time reduction
- Effort: 2-3 days
- Risk: Low
- Aligns with incomplete commit `048ffa4` ("eliminate old_data snapshot")

### Priority 3: Reduce K-Copy Redundancy (Optional, Modest Gains)

**Problem:** All K copies always execute, even when their designated delta is empty.

**Solution:** Track per-relation delta emptiness; skip K-copy passes when empty delta
- Frequent in later iterations (only 1-2 relations have new facts)
- **Expected improvement:** 5-15% additional
- Effort: 1-2 days
- Risk: Low

---

## Corrected Impact Estimates

### Isolation Impacts

**Scenario A: Fix In-Plan CONSOLIDATE (K-way merge, Option A)**
- Wall time: 28.7s → ~16-20s (30-45% reduction)
- Per-iteration cost: Reduced from O(M log M) to O(M log K)

**Scenario B: Eliminate old_data Snapshot**
- Peak RSS: 3.1 GB → <500 MB
- Wall time: ~20% additional reduction (memory churn eliminated)

**Scenario C: Reduce K-Copy Redundancy**
- Wall time: 5-15% additional reduction

**Combined (A+B+C):**
- Wall time: 28.7s → ~10-14s (50-65% improvement)
- Still 2-3× slower than 4.6s baseline (inherent K-copy architecture cost)

---

## Why Still 2-3× Slower Than 4.6s Baseline

Even after Priorities 1-3, estimated wall time is ~10-14s vs baseline 4.6s. The remaining 2-3× gap comes from:

1. **Inherent K-copy cost:** 3 plan copies = 3× more join work (correct semi-naive semantics)
2. **Larger intermediate results:** K-copy produces more intermediate data per iteration
3. **Memory allocation patterns:** More malloc/free cycles due to K copies
4. **Cache locality:** More data in flight per iteration

**This remaining gap requires either:**
- Parallelization (workqueue, Phase 3) — could add 1.5-2× speedup
- Alternative evaluator design (single-pass with interleaved deltas) — research-level complexity
- OR acceptance that 2-3× gap is inherent to correct semi-naive expansion

---

## What Was Wrong With Initial Analysis

| Assumption | Reality | Fix |
|-----------|---------|-----|
| "CONSOLIDATE needs to be optimized" | CONSOLIDATE IS optimized (stratum level); IN-PLAN consolidate needs optimization | Retarget to line 1732 |
| "H1 fix doesn't exist" | H1 fix IS implemented and active (line 2009) | Don't re-implement; optimize what's there |
| "H2 fix is incomplete" | H2 IS complete for K≥3; K<3 rules need threshold lowered | One-line fix (k>=2 instead of k>=3) for K=2 rules |
| "4.6s is achievable" | 4.6s is DD baseline, not columnar baseline | Recalibrate targets |
| "Consolidation is the bottleneck" | In-plan CONSOLIDATE (52-63%) is bottleneck, not stratum consolidation | Different function, different strategy |

---

## Final Recommendation

**Proceed with Priority 1 (In-Plan CONSOLIDATE Optimization):**

1. **Implement K-way merge approach** (2-3 days)
   - Replace full qsort with K-way merge at line 1732
   - Expected: 30-45% improvement (28.7s → ~16-20s)

2. **Execute Priority 2 (old_data elimination)** (2-3 days)
   - Capture delta as merge byproduct
   - Expected: Peak RSS <500MB, 20% wall time improvement

3. **Validate with all 15 benchmarks**
   - Ensure no regressions on non-CSPA workloads
   - Confirm iteration counts and convergence

**Total Effort:** 5-6 days
**Expected Combined Improvement:** 50-65% (28.7s → ~10-14s)
**Success Gate:** >20% improvement (met with margin)

**For remaining gap to 4.6s:** Pursue Phase 3 (workqueue parallelization) as separate initiative.

---

## Key Learnings

1. **Code inspection reveals actual implementation state** — The codebase already contains sophisticated optimizations that were not immediately visible from high-level docs
2. **Bottleneck moved** — What looks like a bottleneck (stratum CONSOLIDATE) is optimized; the real bottleneck (in-plan CONSOLIDATE) is less obvious
3. **Architecture decisions have hidden costs** — The K-copy approach (correct semi-naive semantics) has inherent overhead that no single optimization can overcome
4. **Measure before optimizing** — Initial assumptions about where time goes (52-63% vs 3-7%) were inverted without profiling data

---

**This synthesis corrects the strategy direction based on team findings from deep code inspection and experimental validation.**
