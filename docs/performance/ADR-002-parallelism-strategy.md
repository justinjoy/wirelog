# ADR-002: Benchmark Analysis & Parallelism Strategy

**Status:** Accepted via RALPLAN Consensus
**Date:** 2026-03-07
**Author:** wirelog project team (Planner, Architect, Critic consensus)
**Supersedes/Updates:** ADR-001 Phase A timeline estimate

---

## Summary

After comprehensive consensus-based deliberation, the team has selected **Option C (Hybrid Approach)** for addressing wirelog's performance bottlenecks and parallelism strategy:

**Measure first** → **Optimize algorithm** → **Then parallelize**

This decision replaces speculative optimization with data-driven evidence, corrects the Phase A (global state elimination) effort estimate from "1-2 weeks" to "1-3 days", and establishes concrete gates for committing to implementation work.

---

## Decision

### Selected Option: C (Hybrid)

**Approach:**
1. **Phase 1 (Measure)**: Establish comprehensive benchmark baseline for all 15 workloads + deep-profile CSPA bottleneck (2-3 days)
2. **Phase 2 (Optimize)**: Fix algorithmic inefficiencies in semi-naive evaluation, time-boxed to 5 days with >20% CSPA improvement gate (2-5 days)
3. **Phase 3 (Parallelize)**: Execute Phase A (global state elimination, now 1-3 days) + Phase B-lite (workqueue implementation per ADR-001)

**Decision Rationale:**
- The current bottleneck (CSPA at ~5 seconds) may be algorithmic rather than purely due to single-threaded execution
- Code inspection reveals two concrete inefficiencies: (1) full-relation CONSOLIDATE sort per iteration, (2) incomplete semi-naive delta expansion for multi-way joins
- Measuring profiling data validates or invalidates these hypotheses before committing weeks to workqueue implementation
- Phase A is corrected to 1-3 days (not 1-2 weeks), making hybrid timeline realistic (2-5 weeks total)
- FPGA readiness is preserved: workqueue abstraction is architecturally durable regardless of whether parallelism is the bottleneck

### Rejected Options

**Option A (Workqueue-first):**
- **Invalidation**: While workqueue parallelizes non-recursive strata and benefits all 15 workloads, CSPA (the stated performance pain point) is entirely in a recursive stratum where workqueue provides zero speedup
- **Risk**: 2-4 weeks of development before learning whether the actual bottleneck is addressable by parallelism
- **Data dependency**: Workqueue ROI remains unknown without profiling data on recursive vs non-recursive time fractions
- **Note**: Becomes viable as fallback if profiling shows non-recursive strata dominate CSPA runtime

**Option B (Algorithmic-only):**
- **Invalidation**: Defers FPGA readiness infrastructure (workqueue/threading model) indefinitely, conflicting with Principle #3 (FPGA-aligned architecture)
- **Risk**: Algorithmic optimizations (if successful) may become stale if future architecture changes (e.g., magic sets, lattice-based evaluation) replace the semi-naive loop
- **Data dependency**: The "2-10x improvement" claim for semi-naive tuning is speculative without profiling confirmation
- **Note**: Becomes default path if profiling shows algorithmic issues dominate, but should not be pursued in isolation without threading plan

**Option D (Profiling-only spike):**
- **Subsumed**: Merged into Phase 1 of Option C. Not a standalone strategy.

---

## Decision Drivers

### 1. Actual Bottleneck Location (Unknown without profiling)
- CSPA at ~5 seconds is 400x slower than simple graph workloads
- Code inspection reveals two hypothesized algorithmic issues, but hypothesis ≠ fact
- **Principle**: Measure before optimizing (Principle #1)
- **Implication**: Phase 1 profiling is a prerequisite for all downstream decisions

### 2. Phase A Prerequisite Risk (Reduced from High to Low)
- Original ADR-001 estimated Phase A at "1-2 weeks", blocking all workqueue work
- Code inspection reveals `g_consolidate_ncols` has only 3 occurrences; fix is mechanical (replace `qsort` with context-passing sort)
- **Corrected estimate**: 1-3 days
- **Implication**: Phase A is no longer a blocker; can execute in parallel with profiling/analysis work
- **Consequence**: Option A's cost-benefit ratio improves significantly, but profiling is still needed to assess Option A's impact

### 3. Effort-to-Impact Ratio (Measured by phase duration and payoff scope)
- **Option A (Workqueue)**: 1-4 weeks total effort (Phase A: 1-3 days + Phase B-lite: 2-4 weeks), benefits all 15 workloads' non-recursive strata, zero impact on CSPA's recursive stratum
- **Option B (Algorithmic)**: 1-2 weeks effort, benefits only CSPA (and potentially other recursive workloads), but ROI uncertain without profiling
- **Option C (Hybrid)**: 2-5 weeks total (measure 2-3 days + optimize 2-5 days + Phase A 1-3 days + Phase B contingent on profiling), benefits highest-impact path first
- **Implication**: Hybrid minimizes wasted effort by measuring before large commitments

---

## Alternatives Considered

### Option A: Workqueue-First (Sequential Parallelization)
**Follow ADR-001 as originally scoped: Phase A (global state elimination) → Phase B-lite (CPU workqueue) → Phase C (FPGA integration)**

| Pros | Cons |
|------|------|
| Establishes threading infrastructure (durable for FPGA Phase 4+) | Phase A effort overestimated in ADR-001; now 1-3 days (corrected) |
| Addresses all 15 workloads' non-recursive strata | Zero impact on CSPA's recursive stratum (the stated bottleneck) |
| Work-queue abstraction is architecturally sound | 2-4 weeks before learning if parallelism helps the primary pain point |
| Aligns with FPGA-readiness (Principle #3) | Hypothesis: "workqueue ROI for CSPA is zero" remains unvalidated |
| Low-risk implementation path | Commits significant effort without profiling evidence |

**Hypothesis to be validated**: Non-recursive strata time is negligible compared to recursive stratum for CSPA; workqueue provides <5% overall speedup on the slowest workload.

---

### Option B: Algorithmic Optimization (Optimization-First)
**Focus on semi-naive evaluation improvements: incremental CONSOLIDATE + multi-way join delta expansion**

| Pros | Cons |
|------|------|
| Targets root causes identified in code inspection | Defers FPGA-readiness infrastructure indefinitely (Principle #3 conflict) |
| Likely benefits CSPA most directly (potentially 2-10x) | Algorithmic improvements may become stale if future architecture change occurs |
| Aligns with correctness-first principle (Principle #4) | 1-2 weeks effort before learning impact (ROI unvalidated) |
| Builds expertise in semi-naive evaluation for future magic-sets work | Does not enable multi-worker execution (current `num_workers > 1` returns -1) |
| | Principle #1 (measure-first) partially violated: assumes algorithmic issues are root cause |

**Hypothesis to be validated**: Per-iteration CONSOLIDATE cost (full sort + dedup) and incomplete multi-way join delta expansion are responsible for >80% of CSPA slowness; fixes deliver >20% improvement.

---

### Option C: Hybrid (Measure → Optimize → Parallelize) ✅ SELECTED
**Phase 1: Establish baseline + deep-profile CSPA → Phase 2: Optimize algorithm (5-day time-boxed gate) → Phase 3: Phase A + B-lite**

| Pros | Cons |
|------|------|
| Measure-first (Principle #1): profiling data drives decisions | Process overhead: ADR + consensus documentation (~2 days coordination) |
| Minimal disruption (Principle #2): optimizations only if profiling confirms need | Requires discipline to exit optimization phase at 5-day gate |
| Correct order of investment: fix single-threaded before parallelizing | Timeline uncertainty: depends on profiling findings (2-5 weeks total) |
| FPGA-ready (Principle #3): workqueue infrastructure established before deciding parallelism strategy | Hypothesis-dependent decision (validates or invalidates all options) |
| Correctness-first (Principle #4): algorithmic soundness validated before parallelism | |
| Evidence-gated (Principle #5): phase 2-3 transitions gated on profiling/improvement thresholds | |
| **Corrected Phase A (1-3 days)**: Low-risk unblocking of Phase B | |
| Provides comprehensive baseline for CI + future regression detection | |

**Decision Gates:**
1. **After Phase 1 (measure)**: Profiling data determines whether CSPA is per-iteration-bound (algorithmic) or convergence-bound (iteration count)
   - If per-iteration-bound: Proceed to Phase 2 optimization
   - If convergence-bound: Consider skipping Phase 2, proceeding directly to Phase A + B-lite

2. **After Phase 2 (optimize, max 5 days)**: CSPA wall time improvement threshold >20%
   - If achieved: Establish new single-threaded baseline, proceed to Phase A + B-lite with better baseline
   - If not achieved by day 5: Abandon algorithmic improvements, proceed directly to Phase A + B-lite

3. **After Phase 3 (Phase A execution, 1-3 days)**: Global state elimination complete and validated
   - No behavioral changes, code compiles with `-Wextra`, all tests pass

4. **After Phase B-lite (conditional)**: Workqueue functionality verified
   - `num_workers > 1` produces correct results matching `num_workers=1` baseline
   - Multi-worker latency improves non-recursive strata proportionally

---

## Why Option C Was Chosen

### Alignment with Principles
- **Principle #1 (Measure-first)**: Profiling is first action; all optimizations are data-driven
- **Principle #2 (Minimal disruption)**: Only commits to optimizations if profiling validates them
- **Principle #3 (FPGA-readiness)**: Workqueue infrastructure preserved as future capability
- **Principle #4 (Correctness-first)**: Validates algorithmic soundness before parallelism
- **Principle #5 (Evidence-gated)**: Phase transitions require explicit profiling evidence

### Risk Mitigation
- **Phase A effort risk**: Corrected estimate (1-3 days, not 1-2 weeks) eliminates scheduling blocker
- **Scope creep risk**: 5-day time-box + >20% improvement gate prevents open-ended optimization
- **Hypothesis validation risk**: Profiling data either confirms or refutes algorithmic hypotheses
- **FPGA deferral risk**: Workqueue design is decoupled from optimization decision; can proceed in parallel

### Data-Driven Decision Making
- Profiling Step 1 measures per-iteration cost (CONSOLIDATE domination hypothesis)
- Profiling Step 2 measures delta sizes and iteration counts (incomplete delta expansion hypothesis)
- Profiling Step 3 measures non-recursive vs recursive time fractions (workqueue ROI hypothesis)
- **Result**: ADR-003 (decision after Phase 1) has concrete evidence to either select Option A or commit to Phase 2 optimization

---

## Consequences

### Immediate Consequences (This Iteration)
1. **2-5 week timeline** for full Phase 1-3 completion (vs. 1-4 weeks for Option A, 1-2 weeks for Option B)
2. **Comprehensive baseline documentation** in `docs/performance/benchmark-baseline-2026-03-07.md`
   - All 15 workloads with wall time, peak RSS, iteration count
   - CSPA deep-profile data (per-iteration metrics, delta sizes)
   - Amdahl's law projections for workqueue ROI
3. **ADR-002 acceptance** (this document) replacing speculative decisions with evidence-driven ones
4. **ADR-001 Phase A timeline update** from "1-2 weeks" to "1-3 days" (corrected estimate)

### Phase B (Conditional Consequences)
- If Option A is selected (workqueue prioritized): Phase B-lite design documented in `docs/workqueue/api-sketch.md` proceeds as planned
- If Phase 2 optimization is executed: Commits 2-5 days to incremental CONSOLIDATE + multi-way delta expansion
  - Consequence: New single-threaded baseline established
  - Consequence: Phase B ROI recalculated with improved baseline
- If both optimizations executed: Algorithmic improvements + workqueue creates a compound speedup opportunity

### Phase C+ (Future Consequences)
- FPGA readiness planning (Phase 4+) proceeds with validated data about which bottlenecks are algorithmic vs parallel-addressable
- Work-queue architecture decision (CPU backend, threading model, async interface) is informed by whether parallelism matters for primary workloads

### Process Consequences
- **Consensus pattern established**: RALPLAN-DR structured deliberation (Principles, Drivers, Options, ADR template) is now codified for future architecture decisions
- **Measurement culture**: Comprehensive baseline for all 15 workloads becomes regression-detection baseline for CI
- **Documentation**: `docs/performance/` directory established for performance analysis and decisions

---

## Follow-ups

### Immediate Next Steps (Owner: TBD, Timeline: This Sprint)
1. **Execute Phase 1 (Measure)** — 2-3 days
   - Owner: [Name to be assigned]
   - Deliverable: `docs/performance/benchmark-baseline-2026-03-07.md` with all 15 workloads, CSPA deep-profile data
   - Gate approval: Baseline captures per-iteration metrics, delta sizes, iteration counts; CSPA profiling hypothesis validated or refuted

2. **Generate ADR-003 (Phase 1 Outcome Decision)** — 1 day
   - Owner: [Same as Phase 1 owner, or Architect]
   - Deliverable: ADR-003 selecting Phase 2 (optimize) or Phase 3 (parallelize) based on Phase 1 profiling data
   - Decision gate: Explicit evidence from `docs/performance/benchmark-baseline-2026-03-07.md` used to select next phase

### Phase 2 (Conditional, if ADR-003 selects optimization) — 2-5 days max
- Owner: [To be assigned based on Phase 1 outcome]
- Deliverable: Incremental CONSOLIDATE implementation + multi-way join delta expansion + 5-day gate evaluation
- Gate: >20% CSPA wall time improvement by day 5, or abandon and proceed to Phase 3

### Phase 3 (Parallelize) — 2-5 weeks (after Phase 1, conditional on Phase 2)
- Owner: [To be assigned]
- Deliverable: Phase A (1-3 days) + Phase B-lite (2-4 weeks per ADR-001)
- Baseline: Updated with single-threaded performance after Phase 2 (if executed), or original baseline (if skipped)

### ADR-001 Timeline Update
- Update `docs/workqueue/ADR-001-workqueue-introduction-strategy.md:56` from "Phase A: 1-2 weeks" to "Phase A: 1-3 days"
- Rationale: Code inspection of `g_consolidate_ncols` (3 occurrences, mechanical fix)
- Consequence: Total workqueue timeline becomes 2-4 weeks (Phase A: 1-3 days + Phase B-lite: 2-4 weeks), not 3-6 weeks

### CI/Regression Detection Baseline
- Capture full 15-workload baseline in CI after Phase 1 completion
- Establish per-workload performance assertions (e.g., TC must stay <1ms)
- Track regressions across subsequent commits

---

## Dissenting Views

**None recorded.** Consensus was reached through three rounds of Planner/Architect/Critic review. The Architect's initial critique about Phase A effort overestimation was validated by code inspection and incorporated. All prior concerns were addressed in the revised plan.

---

## Appendix: RALPLAN-DR Summary

### Principles (5)
1. **Measure-first**: Do not optimize without profiling evidence
2. **Minimal disruption**: Constrain optimization scope (5-day time-box)
3. **FPGA-readiness**: Preserve workqueue architecture regardless of optimization path
4. **Correctness-first**: Algorithmic soundness before parallelism
5. **Evidence-gated**: Phase transitions require explicit profiling evidence

### Decision Drivers (3)
1. **Actual bottleneck location**: Unknown without profiling; code inspection suggests but does not prove algorithmic issues
2. **Phase A prerequisite risk**: Corrected from "1-2 weeks" to "1-3 days"; no longer a blocker
3. **Effort-to-impact ratio**: Hybrid approach measures before large optimization/parallelization commitments

### Viable Options (3)
- **Option A**: Workqueue-first (Option A invalidation: CSPA bottleneck is recursive, workqueue has zero direct impact)
- **Option B**: Algorithmic-first (Option B invalidation: defers FPGA infrastructure, ROI speculative)
- **Option C**: Hybrid (Option C selected: measure-first data-driven approach, minimizes wasted effort)

---

## References

- `docs/workqueue/ADR-001-workqueue-introduction-strategy.md` — Phase A original timeline + Phase B-lite design
- `docs/workqueue/api-sketch.md` — Workqueue 5-function interface + recursive strata sequential constraint
- `wirelog/backend/columnar_nanoarrow.c:1065,1072,1112` — `g_consolidate_ncols` global state (Phase A target)
- `wirelog/backend/columnar_nanoarrow.c:1477-1735` — Semi-naive evaluation loop (CSPA bottleneck zone)
- `bench/bench_flowlog.c:594-616` — CSPA workload definition (3 mutually-recursive relations)
- `.omc/plans/benchmark-analysis-and-parallelism-strategy-REVISED.md` — Full detailed plan
- `ARCHITECTURE.md` — Project architecture and FPGA readiness vision (Phase 4+)

---

**Document Version:** 1.0
**Approval Date:** 2026-03-07
**Approvers:** wirelog team (Planner, Architect, Critic consensus via RALPLAN-DR)
