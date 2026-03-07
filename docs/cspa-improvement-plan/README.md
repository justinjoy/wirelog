# CSPA Root Cause Analysis & Improvement Strategy

**Date:** 2026-03-07
**Status:** Complete
**Team:** Distributed analysis (Claude agents + Codex optimization team)

---

## Executive Summary

CSPA (Context-Sensitive Points-to Analysis) exhibits **7.7× performance regression** compared to the historical DD backend (35.3s vs 4.6s). This analysis package identifies the root causes and develops concrete improvement strategies for both bottlenecks.

### Key Findings

1. **Two Distinct Bottlenecks Identified:**
   - **CONSOLIDATE Issue**: Full-relation sort per iteration (O(N log N)) instead of delta-only (O(delta log delta))
   - **Delta Expansion Issue**: Incomplete semi-naive expansion for K=2 rules (missing permutations)

2. **Performance Impact:**
   - CSPA baseline: 35.3s median wall time, 3.0 GB peak RSS
   - Output correctness: Verified (20,381 tuples match baseline)
   - Instruction density: ~15.3M instructions per output tuple

3. **Root Cause Ranking:**
   - Primary: Plan expansion loop overhead (6× from Phase 2C optimization)
   - Secondary: Incremental CONSOLIDATE opportunity (~15-20% gain)
   - Tertiary: Delta expansion incompleteness (K=2 rules, estimated 10-15% iteration reduction)

---

## Document Package

This analysis package includes four comprehensive studies:

### 1. **CSPA-BASELINE-PROFILE.md** (418 lines)
Establishes current performance baseline with empirical measurement.

**Contents:**
- Benchmark metrics (wall time, memory, CPU instructions)
- Rule structure analysis (5 recursive rules, 3 IDB relations)
- Comparison to historical DD backend (7.7× regression)
- Memory amplification analysis (9,300× peak RSS to output ratio)
- Instruction profile (311B instructions per 3 runs, IPC 3.66)

**Key Finding:** 3 GB peak RSS vs 326 KB output = extreme memory amplification

---

### 2. **CONSOLIDATE-IMPROVEMENT-PLAN.md** (596 lines)
Detailed improvement design for the full-relation consolidation bottleneck.

**Contents:**
- Current implementation review (col_op_consolidate_incremental already exists)
- Analysis of 6× regression root cause (not CONSOLIDATE-specific, due to plan expansion loop)
- Four improvement options evaluated:
  - A (Selected): Delta-integrated incremental consolidation (2-3 days effort)
  - B: Hash-based in-plan dedup
  - C: Radix sort optimization
  - D: g_consolidate_ncols fix only (thread safety)
- Detailed pseudocode for Option A approach
- Per-iteration cost breakdown showing O(N) memory allocation and copy overhead
- Expected outcome: Eliminates expensive old_data snapshot copy, reduces peak RSS

**Recommended Action:** Implement Option A (delta-integrated consolidation) to reduce per-iteration memory pressure

---

### 3. **DELTA-EXPANSION-ANALYSIS.md** (302 lines)
Analysis of incomplete semi-naive delta expansion for multi-atom rules.

**Contents:**
- CSPA rule structure with K values (K=IDB atoms in rule body):
  - R1, R4: K=2 rules (NOT expanded, use WL_DELTA_AUTO heuristic)
  - R5: K=3 rule (fully expanded to 3 plan copies)
- Current delta permutation coverage:
  - K=3: Complete (all 3 permutations evaluated)
  - K=2: Incomplete (only δA×B, missing δB×A permutations)
- Impact: K=2 rules delay fact discovery by 1+ iterations per derivation
- Code locations: exec_plan_gen.c:1087 (k >= 3 threshold)

**Key Insight:** Current expansion guard `if (k >= 3)` excludes K=2 rules, causing iteration count penalty

---

### 4. **DELTA-EXPANSION-IMPROVEMENT-PLAN.md** (292 lines)
Concrete implementation strategy for complete semi-naive delta expansion.

**Contents:**
- Plan rewriting design for K=2 rules:
  - Generate 2 plan copies (one per delta position)
  - Copy 0: FORCE_DELTA(pos 0), FORCE_FULL(pos 1)
  - Copy 1: FORCE_FULL(pos 0), FORCE_DELTA(pos 1)
- Evaluator logic (already respects FORCE_DELTA/FORCE_FULL flags, no evaluator changes needed)
- How K copies merge: CONCAT operations + final CONSOLIDATE
- Backward compatibility: No architectural changes, purely plan rewriting

**Recommended Action:** Lower k >= 3 threshold to k >= 2 in exec_plan_gen.c (1 day effort)

---

## Integrated Recommendations

### Phase 1: Delta Expansion Fix (Lower Priority, Lower Effort)
- **Effort:** 1 day (mechanical change: lower k threshold from 3 to 2)
- **Expected Improvement:** 10-15% iteration count reduction for K=2 rules
- **Risk:** Low (plan rewriting already fully tested for K=3)
- **Implementation:** exec_plan_gen.c line 1087
- **Validation:** Run all 15 benchmarks; CSPA should show modest iteration reduction

### Phase 2: CONSOLIDATE Fix (Higher Priority, Medium Effort)
- **Effort:** 2-3 days (extend col_op_consolidate_incremental to emit delta byproduct)
- **Expected Improvement:** 20-30% per-iteration cost reduction, 40-50% peak RSS reduction
- **Risk:** Medium (incremental consolidation already partially implemented, but delta emission is new)
- **Implementation:** columnar_nanoarrow.c col_op_consolidate_incremental()
- **Validation:** Per-iteration instrumentation; verify output correctness unchanged

### Phase 3: Combined Validation
- **Timeline:** Execute both phases sequentially
- **Gate:** Each phase must pass all 15 benchmarks without regression
- **Success Criteria:**
  - CSPA: >20% combined wall time improvement (per consensus decision gate)
  - Memory: Peak RSS reduced by 40%+
  - Other workloads: No regression on TC, Reach, CC, etc.

---

## Technical Debt & Future Work

1. **CSE Materialization** (already implemented in Phase 2C):
   - Progressive materialization of intermediate joins
   - Works synergistically with both CONSOLIDATE and delta expansion fixes

2. **Workqueue Infrastructure** (Phase 3, ADR-001):
   - Requires global state elimination (Phase A: 1-3 days)
   - Enables multi-worker execution (Phase B-lite: 2-4 weeks)
   - Baseline: Apply CONSOLIDATE + delta expansion fixes before starting workqueue

3. **Alternative Strategies**:
   - Incremental join materialization (CSE already explores this)
   - Parallelization of non-recursive strata (workqueue)
   - Alternative join algorithms (hash-based, index-based) - future exploration

---

## Process Notes

### Analysis Methodology
- **Profiling:** Empirical benchmark runs with /usr/bin/time
- **Code Analysis:** Direct examination of columnar_nanoarrow.c and exec_plan_gen.c
- **Architectural Review:** Traced execution paths for K=2 and K=3 rules
- **Comparison:** Historical DD baseline (4.6s) as correctness oracle

### Tool & Configuration
- **Language:** C11 (strict, with -Wall -Wextra -Werror)
- **Build:** Meson + Ninja, Release build (-O2)
- **Benchmarking:** ./build/bench/bench_flowlog with cspa workload
- **Input:** bench/data/cspa (179 facts assign, 20 facts dereference)

### Consensus Alignment
This analysis implements Phase 1 (Measure) of the CONSENSUS-SUMMARY-2026-03-07.md:
- Establishes comprehensive baseline ✓
- Validates/refutes both algorithmic hypotheses ✓
- Generates ADR-003 decision input (Phase 2/3 choice) ✓
- Unblocks Phase 2 optional optimization (5-day time-box, >20% gate) ✓

---

## Success Checklist

- [x] Baseline profile established (CSPA-BASELINE-PROFILE.md)
- [x] CONSOLIDATE bottleneck analyzed (CONSOLIDATE-IMPROVEMENT-PLAN.md)
- [x] Delta expansion bottleneck analyzed (DELTA-EXPANSION-IMPROVEMENT-PLAN.md)
- [x] Improvement plans with implementation details (both documents)
- [x] Impact analysis with prioritization (integrated in this README)
- [x] Consensus-aligned decision gate documented (>20% improvement required)
- [ ] Implementation (next phase, separate effort)
- [ ] Validation across all 15 benchmarks (after implementation)

---

## Document Index

| Document | Lines | Focus | Audience |
|----------|-------|-------|----------|
| CSPA-BASELINE-PROFILE.md | 418 | Empirical baseline metrics | Technical (developers, architects) |
| CONSOLIDATE-IMPROVEMENT-PLAN.md | 596 | Full-relation consolidation fix | Technical (backend optimization) |
| DELTA-EXPANSION-ANALYSIS.md | 302 | Delta expansion incompleteness | Technical (evaluator logic) |
| DELTA-EXPANSION-IMPROVEMENT-PLAN.md | 292 | Delta expansion implementation | Technical (plan generation) |
| README.md (this file) | - | Integrated summary & strategy | Stakeholders, decision-makers |

**Total Analysis:** 1,608 lines of detailed technical investigation

---

## Next Steps

1. **For Stakeholders:** Review this README and integrated recommendations
2. **For Architects:** Read detailed improvement plans (CONSOLIDATE and DELTA-EXPANSION)
3. **For Implementers:** Follow implementation roadmaps in respective improvement plan documents
4. **For Decision-Makers:** Decide Phase 2 disposition (pursue optimization or skip to Phase 3 workqueue)

---

**Generated:** 2026-03-07
**Team:** Claude Code analysis agents (profiler, architect, executor, analyst, verifier)
**Status:** Ready for implementation planning and ADR-003 decision gate
