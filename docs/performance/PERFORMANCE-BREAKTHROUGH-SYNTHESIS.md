# Performance Breakthrough Synthesis

**Date:** 2026-03-08
**Authors:** Quality Reviewer, Codex Specialist, Architect Specialist (3-specialist review team)
**Status:** Final synthesis -- ready for engineering decision

---

## Executive Summary

wirelog's CSPA benchmark regressed 6x (4.6s baseline to 28.7s current) due to sequential K-copy evaluation in the semi-naive evaluator, where K-expanded rules force redundant clone-join-consolidate cycles per iteration. The recommended path forward is **K-fusion**: replacing the sequential K-copy loop with workqueue-based parallel evaluation and inline merge-with-dedup. This leverages existing infrastructure (workqueue, K-way merge code) and requires roughly 400 lines of evaluator refactoring.

However, this synthesis identifies a **critical implementation bug** in the already-written `col_rel_merge_k` function (uses `memcmp` instead of lexicographic int64_t comparison, the exact bug class already fixed once in commit `aba8fc7`), several **unsupported performance claims** (the 30-40% CSPA improvement number lacks rigorous derivation), and **stale documentation** that contradicts the architect-approved strategy. These must be addressed before proceeding.

---

## The Bottleneck: Validated Understanding

### What We Know (Evidence-Based)

The semi-naive evaluator's K-copy expansion pattern is the dominant cost driver. For CSPA (K=2), each of 6 fixed-point iterations evaluates K copies of expanded rules sequentially, producing:
- 12 full-sort consolidate operations across iterations (measured at 28-35% of wall time)
- Sequential join evaluation for K copies (measured at ~12% of wall time)
- Memory pressure from relation cloning and intermediate results

### What We Assume (Needs Validation by Day 3)

1. **K-copy overhead dominates 60-70% of wall time.** The profiling breakdown in BOTTLENECK-PROFILING-ANALYSIS.md cites precise percentages but no profiling tool output is attached. These numbers must be confirmed with `perf record` / `perf report` before implementation begins.

2. **30-40% CSPA improvement from K-fusion.** The architect's own derivation (IMPLEMENTATION-PATHS-ANALYSIS.md lines 259-298) initially calculated only 2.1% improvement from parallelism alone, then revised to 30-40% by appealing to indirect factors (memory pressure, cache locality, FORCE_DELTA overhead). The gap between 2.1% and 30-40% is bridged by assertion, not measurement. This must be validated empirically.

3. **Clone overhead is 2GB.** The architect noted (ARCHITECT-VERIFICATION-FINAL.md line 40) that the VARIABLE operator uses borrowed references, not clones. The actual clone overhead may be lower than claimed.

### What We Know Is Wrong

- **Iterations will NOT reduce from 6 to 1-2.** Fixed-point iteration count is data-dependent (transitive closure depth). K-fusion improves per-iteration cost, not convergence speed. This was a significant error in early documents, corrected by the architect.

---

## Strategy Evaluation: All Paths Considered

### Paths Analyzed in Documents

| Path | Expected Gain | Risk | Verdict |
|------|--------------|------|---------|
| **A: Incremental Consolidate** | 15-20% | High (subtle dedup bugs, state threading) | Insufficient for target; correctness risk |
| **B: K-Fusion Evaluator Rewrite** | 30-40% CSPA, 50-60% DOOP | Medium (well-scoped refactor) | Recommended -- addresses root cause |
| **C: Hybrid (A + B phased)** | 60-70% | Medium | Highest total gain but A becomes throwaway |
| **D: Empty-Delta Skip** | 5-15% | Low | Already designed; additive but insufficient alone |

### Paths NOT Analyzed (Identified in Quality Audit)

These represent potential missed opportunities and should be evaluated before or during implementation:

1. **Radix sort for consolidation.** Current consolidate uses comparison-based `qsort_r` at O(n log n). For fixed-width int64_t rows with 2-4 columns, MSB radix sort achieves O(n * w) which could be substantially faster. Since consolidate is 28-35% of wall time, even a 2x sort speedup yields 14-17% improvement -- and this is orthogonal to K-fusion.

2. **Lazy/deferred consolidation.** Not every relation needs to be fully consolidated every iteration. If a relation's output feeds only one downstream join, consolidation can be deferred until that join requires sorted input. This eliminates unnecessary sorts for relations still accumulating rows.

3. **Stratum-level parallelism.** Independent strata have no data dependencies and can be evaluated in parallel with zero correctness risk. This is orthogonal to K-fusion (which parallelizes within a stratum) and could compound with it.

4. **Join algorithm analysis.** Join operations account for ~12% of wall time but no document analyzes whether the current join algorithm is optimal for the workload characteristics.

### Why K-Fusion Was Selected

The selection is sound for pragmatic reasons:
- It directly addresses the identified root cause (K-copy redundancy)
- It leverages existing infrastructure (workqueue.c: 269 lines, col_op_consolidate_kway_merge: 187 lines)
- The refactoring scope is well-bounded (~400 lines in col_eval_relation_plan)
- It unblocks DOOP (the higher-value target) where K=8 parallelism has more headroom

The selection is less sound on quantitative grounds:
- The 30-40% CSPA improvement claim has a weak derivation
- The primary value proposition is DOOP unblocking, not CSPA improvement
- Alternative paths (radix sort, lazy consolidation) were not evaluated

---

## Recommended Direction: K-Fusion + Required Fixes

### Immediate Prerequisite: Fix memcmp Bug in col_rel_merge_k

**CRITICAL.** The `memcmp`-for-row-comparison bug is more widespread than initially apparent. Commit `aba8fc7` fixed this bug class in the sort/dedup phases, but `memcmp` remains in multiple functions:

1. **`col_rel_merge_k` (line 2138):** 6 call sites (lines 2170, 2190, 2207, 2218, 2229, plus the K>=3 pairwise path). This is the K-fusion merge function -- the central piece of the breakthrough strategy.

2. **`col_op_consolidate_incremental_delta` Phase 2 merge (line 1822):** The merge walk that combines sorted old rows with sorted delta rows uses `memcmp` for ordering comparison. This is in the function that commit `aba8fc7` was supposed to fix -- the sort phase was corrected but the merge phase was missed.

3. **`col_row_in_sorted` (line 3183):** Binary search uses `memcmp` for comparison, but the data it searches was sorted with lexicographic int64_t comparison (`row_cmp_fn` / `kway_row_cmp`). This is a **sort/search mismatch** that will cause binary search to fail on data containing values where memcmp and lexicographic order disagree.

The existing test suites only use small non-negative values where memcmp and lexicographic comparison produce the same result, so these bugs are not caught.

**Fix:** Replace all `memcmp` ordering comparisons with `kway_row_cmp` (already defined at line 1476). The `memcmp` calls used for equality checks only (e.g., dedup where the data is already adjacent and sorted) are safe for equality but not for ordering. Add test cases with endian-sensitive values (negative numbers, values like 254 vs 256).

### Recommended Implementation Sequence

**Phase 0: Fix and Validate (Day 0, 2-4 hours)**
1. Fix memcmp bug in col_rel_merge_k
2. Add regression test with endian-sensitive values (e.g., 254 vs 256, negative vs positive)
3. Run full regression suite
4. Profile CSPA with `perf record` to establish actual baseline breakdown

**Phase 1: K-Fusion Core (Days 1-3, ~16 hours)**
1. Design K-fusion plan node (COL_OP_K_FUSION metadata structure)
2. Modify exec_plan_gen.c to detect K-copy relations and emit K-fusion nodes
3. Implement col_op_k_fusion() dispatch: workqueue submission, barrier, merge
4. Per-worker arena allocation pattern

**Phase 2: Validation (Days 4-5, ~8 hours)**
1. All 15 workloads pass regression (output correctness gate)
2. Iteration count verified at 6 for CSPA (must not increase)
3. CSPA wall-time profiled (target: 30-40% improvement, accept 20%+)
4. Workqueue overhead < 5% on K=2
5. DOOP validation (primary breakthrough metric: < 5 minutes)

**Phase 3: DOOP-First Optimization (Days 6-10, if needed)**
1. If DOOP completes: measure and optimize
2. If DOOP still times out: investigate other blockers (join complexity, relation explosion)
3. If CSPA improvement < 20%: investigate whether K-copy truly dominates (revalidate root cause)

### Document Cleanup (Parallel Track)

Before implementation starts, clean up the document contradictions:
- Archive or update EXECUTIVE-BREAKTHROUGH-SUMMARY.md (still has pre-architect numbers)
- Remove inline "working through math" from IMPLEMENTATION-PATHS-ANALYSIS.md
- Standardize path labels (A/B/C) across all documents
- Update K-FUSION-DESIGN.md function signatures to match implementation

---

## Feasibility: Realistic or Optimistic?

### Timeline Assessment

| Estimate Source | Duration | Team | Total Hours |
|----------------|----------|------|-------------|
| IMPLEMENTATION-PATHS-ANALYSIS.md | 27 hours | 1 engineer | 27h |
| ARCHITECT-VERIFICATION-FINAL.md | 24-38 hours | 1 engineer | 24-38h |
| BREAKTHROUGH-STRATEGY-ROADMAP.md | 2-3 weeks | 1 engineer | 100-150h |
| EXECUTIVE-BREAKTHROUGH-SUMMARY.md (STALE) | 4 weeks | 2 engineers | 560h |

**Realistic assessment:** The core refactoring is 24-38 hours of focused work (architect estimate). Adding testing, profiling, and DOOP validation brings it to 40-60 hours. The 2-3 week calendar time is realistic given interruptions and iteration on findings.

The 560-hour estimate from the executive summary is obsolete and should be disregarded.

### Risk Summary

| Risk | Probability | Impact | Status |
|------|------------|--------|--------|
| **memcmp bug produces wrong merge results** | HIGH (confirmed in code) | CRITICAL | Must fix before any K-fusion deployment |
| **CSPA improvement < 20%** | MEDIUM | HIGH | Indicates root cause misidentified; pivot to alternative paths |
| **Workqueue overhead > 5% on K=2** | MEDIUM (1 in 3) | MODERATE | Fallback: sequential K for CSPA, parallel for DOOP only |
| **DOOP still times out** | MEDIUM (1 in 3) | MODERATE | K-fusion necessary but may not be sufficient |
| **Iteration count increases** | LOW (1 in 20) | CRITICAL | Indicates correctness regression; rollback immediately |
| **K-copy does not dominate 60-70%** | LOW-MEDIUM | HIGH | Strategy based on assumption; must validate with perf |

---

## Expected Outcomes

### CSPA (K=2)

| Scenario | Wall Time | Improvement | Probability |
|----------|-----------|-------------|-------------|
| **Optimistic** | 17-18s | 35-40% | 20% |
| **Realistic** | 20-23s | 20-30% | 50% |
| **Conservative** | 24-27s | 5-15% | 25% |
| **Regression** | >28.7s | Negative | 5% |

The wide range reflects that the 30-40% claim rests on unvalidated profiling data. The realistic scenario accounts for the possibility that K-copy overhead is lower than 60-70% of wall time.

### DOOP (K=8)

| Scenario | Outcome | Probability |
|----------|---------|-------------|
| **Completes < 3 min** | Full breakthrough | 30% |
| **Completes < 5 min** | Primary target met | 40% |
| **Still times out** | K-fusion necessary but insufficient | 30% |

DOOP is the higher-value target. K=8 parallelism provides more speedup headroom than K=2, making this the more likely breakthrough.

### Iteration Count

Will stay at 6 for CSPA. This is a data-dependent property of the transitive closure depth, not addressable by K-fusion or any algorithmic optimization short of changing the evaluation strategy entirely.

### Risk Level: YELLOW

- GREEN factors: Sound architecture, existing infrastructure, well-scoped refactor
- YELLOW factors: Unvalidated performance claims, confirmed code bug, document inconsistencies
- RED factors: None identified

---

## Implementation Sequence: DOOP-First Validation

The architect correctly identifies DOOP as the primary breakthrough target. Recommended sequence:

1. **Fix memcmp bug** (blocker -- must be done first regardless of strategy)
2. **Profile CSPA baseline** with `perf` (validate the 60-70% K-copy dominance claim)
3. **Implement K-fusion dispatch** (core refactoring)
4. **Validate on CSPA first** (regression safety -- easier to debug on smaller workload)
5. **Run DOOP** (primary breakthrough metric)
6. **Optimize based on profiling** (data-driven, not assumption-driven)

---

## Success Criteria (Day-by-Day Validation Gates)

### Day 0: Bug Fix Gate
- [ ] memcmp replaced with kway_row_cmp in col_rel_merge_k (all 6 call sites)
- [ ] Test case added with endian-sensitive values (e.g., 254 vs 256)
- [ ] All existing tests still pass
- [ ] `perf record` baseline captured for CSPA

### Day 3: Assumption Validation Gate
- [ ] K-copy overhead measured with `perf` (is it really 60-70%?)
- [ ] Iteration count confirmed at 6 (no change)
- [ ] Workqueue overhead < 5% on K=2
- [ ] If K-copy < 50%: STOP and reassess strategy

### Day 5: CSPA Regression Gate
- [ ] All 15 workloads pass (output correctness)
- [ ] CSPA wall-time measured (accept any improvement > 15%)
- [ ] Memory RSS < 4GB
- [ ] No new compiler warnings

### Day 7: DOOP Breakthrough Gate
- [ ] DOOP attempted with 5-minute timeout
- [ ] If completes: measure time and declare breakthrough
- [ ] If times out: document remaining blockers, plan next phase

### Day 10: Production Readiness
- [ ] All tests pass including new K-fusion tests
- [ ] Code formatted (clang-format llvm@18)
- [ ] ARCHITECTURE.md updated
- [ ] Performance results documented with `perf` evidence

---

## Top Risks and Mitigation

### Rank 1: memcmp Bug in Merge Code
- **Impact:** Silent data corruption in merged results on little-endian systems
- **Mitigation:** Fix immediately. Replace with kway_row_cmp. Add endian-sensitive test cases.
- **Owner:** First engineering task before any K-fusion work

### Rank 2: Performance Claims Based on Unvalidated Profiling
- **Impact:** If K-copy overhead is 30% (not 60-70%), improvement will be 10-15% (not 30-40%)
- **Mitigation:** Profile with `perf record` on Day 0. Make go/no-go decision based on actual data.
- **Owner:** Engineer, Day 0

### Rank 3: DOOP May Have Non-K-Copy Bottlenecks
- **Impact:** K-fusion is necessary but not sufficient; DOOP still times out
- **Mitigation:** If DOOP fails, investigate join complexity, relation explosion, plan rewriting issues
- **Owner:** Engineer, Day 7+

### Rank 4: Document Inconsistencies Mislead Implementation
- **Impact:** Engineer reads stale doc, implements wrong approach or sets wrong targets
- **Mitigation:** Archive EXECUTIVE-BREAKTHROUGH-SUMMARY.md. Designate SPECIALIST-REVIEW-SYNTHESIS.md + this document as canonical references.
- **Owner:** Project lead, before kickoff

### Rank 5: Workqueue Overhead on Small K
- **Impact:** K=2 parallelism adds overhead instead of saving time on CSPA
- **Mitigation:** Measure on Day 3. Fallback: sequential K for CSPA, parallel for DOOP (K=8)
- **Owner:** Engineer, Day 3 gate

---

## Post-Breakthrough Roadmap

### If K-Fusion Succeeds (DOOP < 5 Minutes)

**30-day horizon:**
- Optimize DOOP hot paths based on profiling data
- Evaluate radix sort for consolidation (potential additional 10-15% on sort-heavy workloads)
- Investigate stratum-level parallelism (orthogonal, compounds with K-fusion)

**60-day horizon:**
- Phase 3 multi-way join optimization (now unblocked by DOOP completion)
- Workqueue-based evaluation for non-recursive strata
- Lazy consolidation analysis

**90-day horizon:**
- Production deployment with full performance regression suite
- Automated performance tracking in CI
- Evaluate whether baseline (4.6s) is recoverable or whether current architecture has a structural floor

### If K-Fusion Does Not Hit Targets

**Fallback 1: CSPA improvement < 20%**
- Root cause is not K-copy dominance as assumed
- Investigate: join algorithm, consolidate sort algorithm (radix sort), memory allocator overhead
- Timeline: +2 weeks for profiling and alternative implementation

**Fallback 2: DOOP still times out**
- K-fusion addresses K-copy overhead but DOOP has additional bottlenecks
- Investigate: relation explosion (row count growth across iterations), join selectivity, plan rewriting for DOOP-specific rules
- Timeline: +3-4 weeks for DOOP-specific investigation and optimization

**Fallback 3: Workqueue overhead negates parallelism gains**
- Sequential K-fusion (merge without parallelism) still eliminates redundant consolidates
- Reduced but nonzero benefit (est. 10-15%)
- Consider: thread pool warmup, larger task granularity, batched K evaluation

---

## Appendix: Document Status

| Document | Status | Action Required |
|----------|--------|-----------------|
| BOTTLENECK-PROFILING-ANALYSIS.md | Partially stale | Update improvement targets to 30-40% |
| IMPLEMENTATION-PATHS-ANALYSIS.md | Contains working notes | Remove inline "working through math" section |
| BREAKTHROUGH-STRATEGY-ROADMAP.md | Partially updated | Fix contradictions (team size, CSPA target) |
| K-FUSION-DESIGN.md | Stale function signatures | Update to match actual implementation |
| IMPLEMENTATION-TASK-BREAKDOWN.md | Current | Add memcmp fix as Task 0 |
| ARCHITECT-VERIFICATION-FINAL.md | Current | No changes needed |
| K-FUSION-ARCHITECTURE.md | Partially stale | Fix memcmp/lexicographic contradiction, K>=3 algorithm description |
| EXECUTIVE-BREAKTHROUGH-SUMMARY.md | STALE | Archive or rewrite -- contradicts architect review |
| SPECIALIST-REVIEW-SYNTHESIS.md | Current | No changes needed |
| CONSOLIDATE-COMPLETION.md | Current | Note: reports 30.7s CSPA (vs 28.7s elsewhere) |
