# Benchmark Analysis & Parallelism Strategy — Consensus Summary

**Date:** 2026-03-07
**Process:** RALPLAN Consensus Deliberation (Planner → Architect → Critic)
**Status:** Approved (2 iterations)

---

## Quick Summary

**DECISION: Proceed with Option C (Hybrid: Measure → Optimize → Parallelize)**

- **Phase 1 (Measure)**: 2-3 days — Establish comprehensive benchmark baseline + deep-profile CSPA bottleneck
- **Phase 2 (Optimize)**: 2-5 days (max) — Fix algorithmic inefficiencies IF profiling confirms they matter (5-day time-box + >20% improvement gate)
- **Phase 3 (Parallelize)**: 2-5 weeks — Execute Phase A (global state elimination, corrected to 1-3 days) + Phase B-lite (workqueue per ADR-001)

**Total Timeline:** 2-5 weeks (if optimization is needed) to 3-5 weeks (if optimization succeeds) to 4-6 weeks (if both phases execute)

---

## Key Findings from Consensus Discussion

### Finding #1: Phase A Effort Overestimated in ADR-001
**Original claim**: "Phase A (global state elimination) requires 1-2 weeks"
**Actual finding**: `g_consolidate_ncols` appears at only 3 locations in code; fix is mechanical (`qsort` → context-passing sort)
**Corrected estimate**: **1-3 days**
**Impact**: Phase A is no longer a scheduling blocker; can execute in parallel with profiling/analysis

### Finding #2: CSPA Bottleneck Has Two Hypothesized Algorithmic Issues
1. **Full-relation CONSOLIDATE per iteration** — Current code sorts + deduplicates ALL accumulated facts every iteration (O(N_total log N_total) per iteration), not just the delta (O(delta log delta))
2. **Incomplete semi-naive delta expansion** — Multi-way joins (3+ atoms) only apply ONE delta substitution per iteration (delta(A) × B × C), missing permutations that should reduce iteration count

**Status**: Hypotheses until profiling validates them
**Impact**: If both are confirmed, could provide 2-10x CSPA speedup

### Finding #3: Workqueue ROI for CSPA is Bottleneck-Dependent
- Workqueue (Option A) parallelizes **non-recursive strata** only
- CSPA is a **single recursive stratum** with 3 mutually-recursive relations
- **Implication**: If CSPA bottleneck is entirely recursive (as code structure suggests), workqueue provides zero speedup for the slowest workload
- **Caveat**: If ~20% of CSPA time is in non-recursive setup/coordination, workqueue still provides marginal benefit

**Status**: Unknown until Phase 1 profiling measures time breakdown
**Impact**: Profiling data determines whether Option A or Options B/C are viable

### Finding #4: Three Genuinely Viable Options (After Architect Critique)
| Option | Approach | Timeline | Bottleneck Assumption | FPGA-Ready |
|--------|----------|----------|----------------------|-----------|
| **A** (Workqueue-first) | Implement Phase A + B-lite immediately | 1-4 weeks | Parallelism is bottleneck | ✓ Yes |
| **B** (Algorithmic-first) | Fix CONSOLIDATE + delta expansion | 1-2 weeks | Algorithm is bottleneck | ✗ No (defers infrastructure) |
| **C** (Hybrid) ✅ **SELECTED** | Measure first, then optimize conditionally, then parallelize | 2-6 weeks | Data-driven (measure first) | ✓ Yes |

**Architect's critique**: Option A risks 2-4 weeks wasted if parallelism is not the bottleneck. Option B violates FPGA-readiness principle (Principle #3). Option C respects all 5 principles.

---

## Why Option C Won

### Principle Alignment (All 5 Respected)
1. **Measure-first** ✓ — Profiling data drives phase-2 and phase-3 decisions
2. **Minimal disruption** ✓ — Algorithmic optimizations only if validated; 5-day time-box prevents scope creep
3. **FPGA-readiness** ✓ — Workqueue design preserved; threading model remains architecturally sound
4. **Correctness-first** ✓ — Algorithmic soundness established before parallelism
5. **Evidence-gated** ✓ — Phase transitions require explicit profiling evidence

### Risk Mitigation
- **Phase A no longer blocks**: Corrected to 1-3 days (was 1-2 weeks)
- **Scope creep gated**: 5-day time-box for Phase 2 optimization with >20% improvement threshold
- **Hypotheses validated**: Profiling either confirms or refutes both algorithmic issues and workqueue ROI
- **No wasted effort**: If Phase 1 shows non-recursive strata are negligible, skip Phase 2 and go straight to Phase A + B-lite

---

## What Happens Next

### Immediately (This Sprint)
1. **Execute Phase 1 (Measure)** — 2-3 days
   - Run full 15-workload benchmark suite with peak RSS and iteration count capture
   - Deep-profile CSPA with per-iteration instrumentation (wall time, delta sizes, fact counts)
   - Output: `docs/performance/benchmark-baseline-2026-03-07.md`
   - Decision gate: Profiling data validates/refutes the two algorithmic hypotheses

2. **Generate ADR-003** — 1 day
   - Based on Phase 1 results, decide: Skip Phase 2 (go to Phase 3) OR Commit to Phase 2 (5-day optimization attempt)
   - Explicit evidence required (from `benchmark-baseline-2026-03-07.md`)

### Phase 2 (Conditional — If ADR-003 Selects Optimization)
- **Owner**: [TBD based on Phase 1 findings]
- **Work**: Incremental CONSOLIDATE + multi-way join delta expansion
- **Gate**: >20% CSPA wall time improvement by day 5, or abandon
- **Output**: Optimized columnar backend + updated baseline

### Phase 3 (Unconditional — After Phases 1-2)
- **Owner**: [TBD]
- **Work**: Phase A (1-3 days) + Phase B-lite (2-4 weeks, per ADR-001)
- **Baseline**: Original or Phase-2-optimized baseline (depending on Phase 2 outcome)
- **Output**: Multi-worker columnar backend with CPU workqueue

---

## Metrics & Acceptance Criteria

### Phase 1 Success Criteria
- [ ] Benchmark binary runs all 15 workloads without timeout
- [ ] `benchmark-baseline-2026-03-07.md` captures: wall time (min/median/max), peak RSS, iteration count for all 15 workloads
- [ ] CSPA deep-profile data captured: per-iteration wall time, delta sizes, new fact counts
- [ ] Amdahl's law projections calculated for workqueue ROI based on recursive/non-recursive time fractions

### Phase 2 Success Criteria (if executed)
- [ ] CONSOLIDATE incremental sort implemented + tested
- [ ] Multi-way join delta expansion implemented for 3+ atom rules
- [ ] CSPA wall time improves >20% on baseline (or Phase 2 is abandoned by day 5)
- [ ] All 15 benchmarks still pass with new implementation
- [ ] New baseline documented

### Phase 3 Success Criteria
- [ ] Phase A: `g_consolidate_ncols` global state eliminated (1-3 days)
- [ ] Phase B-lite: Workqueue 5-function interface implemented + CPU backend (2-4 weeks)
- [ ] `num_workers > 1` produces correct results matching `num_workers=1` baseline
- [ ] Multi-worker latency improves non-recursive strata proportionally

---

## Decision Drivers (Why This Approach)

1. **Actual bottleneck is unknown** — Code inspection reveals hypotheses but not proof; profiling is the only way to be sure
2. **Phase A is now low-cost** — Corrected effort (1-3 days) eliminates the "blocking blocker" that made Option A look risky
3. **Optimization ROI is unknowable without profiling** — The 2-10x improvement claim for algorithmic fixes is data-dependent; waste weeks chasing it only if profiling validates it
4. **All phases are now de-risked by gating** — Phase 2 is time-boxed; phase transitions require explicit evidence

---

## Trade-Offs Acknowledged

| What We're Trading | Why | Cost | Benefit |
|-------------------|-----|------|---------|
| **Timeline (2-6 weeks vs 1-4 weeks for Option A)** | Measure before optimizing (Principle #1) | +2 weeks if Phase 2 is needed | De-risks both Options A and B; validates hypotheses |
| **Process overhead (ADR + consensus)** | RALPLAN structure for architecture decisions | ~2 days coordination | Permanent decision pattern; prevents future scope creep |
| **Optimization scope uncertainty** | Phase 2 outcome is unknown | Up to 5 days if it fails | Only commits if profiling validates need |
| **ADR-001 timeline conflict** | Correcting Phase A estimate requires updating ADR-001 | Minor docs update | More accurate planning for all future phases |

---

## Open Questions for Team Discussion

1. **Who owns Phase 1 profiling?** Recommend: Someone familiar with benchmark infrastructure + C internals
2. **What improvement threshold for Phase 2?** Current consensus: >20% CSPA wall time. Accept or revise?
3. **If Phase 2 optimization succeeds, do we measure workqueue ROI before/after?** Yes recommended (establishes what parallelism adds to optimized baseline)
4. **Should Phase 3 be split (Phase A separate from Phase B-lite)?** Current: Keep together for schedule coherence; Phase A (1-3 days) is too small to parallel-track

---

## Document References

- **Full Plan**: `.omc/plans/benchmark-analysis-and-parallelism-strategy-REVISED.md` (detailed steps, guardrails, verification)
- **Architecture Decision Record**: `ADR-002-parallelism-strategy.md` (formal decision with principles, drivers, alternatives, consequences)
- **Related ADRs**:
  - `docs/workqueue/ADR-001-workqueue-introduction-strategy.md` (Phase A/B-lite design, updated timeline)
  - `docs/workqueue/api-sketch.md` (workqueue interface + CPU backend design)
- **Architecture**: `ARCHITECTURE.md` (FPGA Phase 4+ vision)

---

## Next Step: Schedule Phase 1

**Recommendation**: Assign Phase 1 (Measure) owner this sprint, with 2-3 day time-box. Phase 1 output unblocks ADR-003 (phase 2/3 decision) immediately.

**Contact**: Team lead for owner assignment and Phase 1 kick-off.
