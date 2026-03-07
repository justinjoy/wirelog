# Team Discussion: Benchmark Analysis & Parallelism Strategy

**Date:** 2026-03-07
**Format:** RALPLAN Consensus Deliberation (Planner → Architect → Critic → Consensus)
**Participants:** wirelog team (via structured agent consensus)
**Status:** Approved — Consensus Reached (Option C)

---

## Discussion Summary

The team conducted a structured 3-round consensus deliberation to decide whether implementing workqueue would improve wirelog's benchmark performance or if an alternative approach is needed.

**Starting Question**: Given that CSPA (Context-Sensitive Points-to Analysis) completes in ~5 seconds — 400x slower than simple graph workloads — should we:
1. Implement workqueue parallelism immediately (Option A)?
2. Fix algorithmic inefficiencies first (Option B)?
3. Measure the bottleneck first, then decide (Option C)?

**Result**: Consensus on **Option C (Hybrid: Measure → Optimize → Parallelize)**

---

## Round 1: Planner's Initial Deliberation

### RALPLAN-DR Summary Generated
**Principles** (5):
1. Measure-first (data-driven decisions)
2. Minimal disruption (scope control)
3. FPGA-readiness (architecturally durable decisions)
4. Correctness-first (algorithmic before parallel)
5. Evidence-gated (phase transitions require proof)

**Decision Drivers** (3):
1. Actual bottleneck location is unknown without profiling
2. Phase A (global state elimination) initially estimated at 1-2 weeks, potentially blocking
3. Effort-to-impact ratio differs dramatically across options

**Viable Options** (4):
- **A**: Workqueue-first (implement Phase A + B-lite immediately)
- **B**: Algorithmic-first (fix semi-naive evaluation issues)
- **C**: Hybrid (measure first, then decide)
- **D**: Profiling-only spike (subsumed into C's Phase 1)

**Planner's Recommendation**: Option C (measure-first preserves all principles, minimizes waste)

---

## Round 2: Architect's Critical Review

### Steelman Antithesis (Strongest Argument Against Option C)

> "The plan is overengineered for a profiling exercise. Five steps, three options, and an ADR template for what is fundamentally 'run benchmarks, look at numbers, pick a direction' could be collapsed to two steps. The ceremony risks creating an illusion of rigor while the decision will likely be obvious once profiling data exists."

### Architect's Key Findings

**Finding #1: Phase A Effort Drastically Overestimated**
- ADR-001 claims Phase A (global state elimination) requires 1-2 weeks
- Code inspection reveals `g_consolidate_ncols` appears at exactly 3 locations
- Fix is mechanical: replace `qsort` with context-passing sort (1-3 days, not 1-2 weeks)
- **Impact**: Phase A is no longer a scheduling blocker; Option A becomes much more viable

**Finding #2: Two Concrete Algorithmic Issues Identified**
- **Issue A**: Full-relation CONSOLIDATE every iteration (O(N_total log N_total)) instead of incremental delta sort (O(delta log delta))
- **Issue B**: Incomplete semi-naive delta expansion — multi-way joins only apply one delta substitution per iteration, missing permutations
- **Status**: Hypotheses until validated by profiling, but code inspection makes them plausible

**Finding #3: Workqueue Has Zero Impact on CSPA's Recursive Stratum**
- CSPA is a single recursive stratum with 3 mutually-recursive relations
- Workqueue only parallelizes non-recursive strata (per api-sketch.md:241-243)
- **Critical consequence**: If CSPA bottleneck is entirely recursive, workqueue provides zero speedup for the slowest workload
- **Hypothesis**: CSPA bottleneck is per-iteration-dominated (algorithmic), not parallelism-limited

### Architect's Recommendation
> **Option C with scope tightening.** Hybrid path is correct, but:
> 1. Shrink profiling spike from 1 week to 2 days (targeted instrumentation, not exploratory)
> 2. Execute Phase A immediately in parallel (1-3 days, not 1-2 weeks)
> 3. Prioritize incremental CONSOLIDATE + multi-way join delta expansion
> 4. Gate workqueue on post-optimization profiling showing parallelizable strata still dominate

**Tradeoff Tension Identified**: Principle #1 (measure-first) vs Principle #4 (correctness-first). Code review reveals likely bottlenecks, but profiling is needed to measure actual costs. Plan should prioritize targeted validation, not exploratory profiling.

---

## Round 3: Critic's Quality Evaluation

### Initial Verdict
**ITERATE** — Plan is sound but has gaps in acceptance criteria, lacks concrete gates, and doesn't address Architect's Phase A correction.

### Critical Gaps Found

**Gap #1: Acceptance Criteria Too Vague**
- Original: "Team discussion reaches consensus" (unmeasurable)
- Revised: "Team selects ONE of A/B/C, ADR-002 records dissenting views, each reviewer records explicit approve/reject"
- **Impact**: Makes decision unambiguous and documented

**Gap #2: Phase A Effort Overestimate Not Incorporated**
- Architect identified 1-3 day correction; plan still referenced 1-2 weeks
- **Impact**: Effort-to-impact calculations were stale
- **Fix**: Revise timeline estimates across all options

**Gap #3: Option C Time-Box Missing**
- Original: "Requires discipline to not over-optimize" (vague)
- Revised: "Algorithmic phase capped at 5 working days with >20% CSPA improvement threshold"
- **Impact**: Prevents scope creep; makes phase 2-3 transition gated and concrete

**Gap #4: Step 1 Execution Details Incomplete**
- Missing: Data paths for multi-relation workloads (CSPA, Polonius)
- Missing: Peak RSS capture method (platform-specific: `/usr/bin/time -v` on Linux, `time -l` on macOS)
- Missing: Iteration count capture (needs instrumentation or log parsing)
- **Fix**: Add concrete paths and tool specifications

**Gap #5: Guardrails Contradiction**
- Original: "Must NOT change columnar_nanoarrow.c" vs Step 2 requirement to instrument it
- Revised: "Temporary instrumentation permitted if gated behind `#ifdef WL_PROFILE`"
- **Impact**: Clarifies intent (no behavioral changes, only temporary profiling code)

### Critic's Recommendation After Revision
**APPROVE** — Plan addresses all critical gaps. Consensus ready.

> "Plan is ready for implementation. The consensus deliberation has concluded with agreement on: Option C (Hybrid) with Phase A effort reduced to 1-3 days, Option C time-boxed to 5 days with >20% CSPA improvement gate, and explicit measurement-driven gating before option selection."

---

## Key Agreements (Team Consensus)

### Agreement #1: Option C (Hybrid) Defeats All Alternatives
| Option | Consensus Position |
|--------|------------------|
| **A (Workqueue-first)** | **Rejected**: Risks 2-4 weeks without knowing if parallelism solves CSPA. Viable only if profiling shows non-recursive strata dominate. Phase A correction (1-3 days) makes A more competitive, but profiling is still prerequisite. |
| **B (Algorithmic-first)** | **Rejected**: Violates FPGA-readiness principle (defers threading infrastructure). Algorithmic improvements are speculative without profiling validation. 2-10x improvement claim is data-dependent. |
| **C (Hybrid)** | **Consensus selected**: Measure-first respects all 5 principles. Scope-gated (5-day time-box + >20% threshold). De-risks both A and B by providing data. Workqueue infrastructure preserved. |

### Agreement #2: Phase A Is No Longer a Blocker
- **Original ADR-001 estimate**: 1-2 weeks (was perceived as scheduling blocker)
- **Actual code inspection**: 3 occurrences of `g_consolidate_ncols`, mechanical fix
- **Corrected estimate**: 1-3 days
- **Implication**: Can execute in parallel with profiling work; no longer a bottleneck
- **Action**: Update ADR-001 timeline documentation

### Agreement #3: Two Algorithmic Hypotheses Must Be Validated by Profiling
1. **CONSOLIDATE hypothesis**: Full-relation sort per iteration is per-iteration bottleneck (O(N log N) per iteration)
   - **Validation method**: Per-iteration instrumentation measuring CONSOLIDATE cost
   - **Expected finding**: CONSOLIDATE dominates per-iteration time

2. **Delta expansion hypothesis**: Incomplete semi-naive delta expansion increases iteration count
   - **Validation method**: Per-iteration measurement of delta sizes and iteration counts
   - **Expected finding**: Incomplete expansion causes 2,000+ iterations instead of 1,000+

**Consequence**: If both hypotheses validate, Phase 2 optimization becomes likely profitable (2-10x gain possible).

### Agreement #4: Profiling Must Measure Non-Recursive vs Recursive Time
- **Question**: How much of CSPA runtime is in non-recursive strata vs recursive evaluation?
- **Why it matters**: Workqueue ROI depends on this ratio
- **Measurement**: Amdahl's law calculation from Phase 1 profiling data
- **Decision gate**: If non-recursive < 10% of time, workqueue ROI is low; prioritize Option B (algorithmic)

### Agreement #5: Time-Boxes Are Non-Negotiable
- **Phase 2 (Optimization)**: 5 days maximum with >20% CSPA improvement threshold
  - **Rationale**: Prevents open-ended optimization efforts; gates to explicit evidence
  - **Fallback**: If no >20% improvement by day 5, abandon optimization and go to Phase 3

- **Phase 1 (Profiling)**: 2-3 days maximum
  - **Rationale**: Targeted instrumentation, not exploratory profiling
  - **Deliverable**: `docs/performance/benchmark-baseline-2026-03-07.md` with all metrics

---

## Dissenting Views

**None recorded.** All three consensus agents (Planner, Architect, Critic) converged on Option C after two rounds of deliberation and revision. The Architect's initial concerns about Phase A effort overestimation were validated by code inspection and incorporated into the revised plan.

---

## Decision Gates (Explicit Conditions for Phase Transitions)

### Gate 1: Phase 1 → ADR-003 (Decision after profiling)
**Condition**: Profiling data from Phase 1 must answer:
- Q1: Is CONSOLIDATE the per-iteration bottleneck? (measure CONSOLIDATE cost per iteration)
- Q2: Is delta expansion incomplete for multi-way joins? (measure iteration count vs theoretical minimum)
- Q3: What fraction of CSPA time is in non-recursive strata? (Amdahl's law calculation)

**ADR-003 Decision Options**:
- **Path A**: If profiling shows non-recursive is >20% and workqueue ROI is clear → Skip Phase 2, go directly to Phase 3 (parallelize)
- **Path B**: If profiling shows per-iteration algorithmic issues → Commit to Phase 2 (5-day optimization attempt)
- **Path C**: If profiling is inconclusive → Execute both Phase 2 and Phase 3 (maximize hedge)

### Gate 2: Phase 2 → Phase 3 (Optimization outcome)
**Condition**: CSPA wall time improvement >20% by day 5
- **Pass**: Establish new single-threaded baseline, proceed to Phase 3 with improved performance
- **Fail**: Abandon optimization, proceed directly to Phase 3 with original baseline

### Gate 3: Phase 3 Completion (Parallelization outcome)
**Condition**: Multi-worker correctness and latency improvement
- **Pass**: `num_workers > 1` produces byte-for-bit matching results vs `num_workers=1`, non-recursive strata latency improves proportionally
- **Fail**: Investigate regression, roll back workqueue

---

## Recommended Team Actions

### For Sprint Planning (This Sprint)
1. **Assign Phase 1 owner** (Recommend: Someone with C internals + benchmark infrastructure knowledge)
2. **Schedule Phase 1 kick-off** (2-3 day effort)
3. **Create ADR-003 pre-draft template** (ready for Phase 1 results)

### For Next Sprint (Conditional on ADR-003 decision)
1. **If Path A (parallelize immediately)**: Assign Phase 3 owner for Phase A + B-lite
2. **If Path B (optimize first)**: Assign Phase 2 owner for 5-day optimization sprint
3. **If Path C (both)**: Assign Phase 2 owner + Phase 3 owner (parallel tracks if resources allow)

### For Documentation
1. **Update ADR-001** timeline (Phase A: 1-3 days, not 1-2 weeks)
2. **Create benchmark baseline CI check** (regression detection once Phase 1 establishes baseline)
3. **Archive this consensus summary** for future reference (RALPLAN pattern for architecture decisions)

---

## Lessons from This Deliberation

### What Went Right
1. **Architect's code inspection** caught critical Phase A overestimate (1-2 weeks vs 1-3 days)
2. **Hypothesis-driven framing** prevents premature optimization commitments
3. **Time-box gates** prevent scope creep without being overly rigid
4. **Principle-based decision** (5 principles) ensures FPGA-readiness is preserved regardless of outcome

### What Could Improve
1. **Earlier code inspection**: Could have happened before planning, reducing planning rework
2. **Tighter discovery scope**: Planner's initial plan was broader than needed; targeted investigation narrowed it
3. **Clearer acceptance criteria from the start**: Critic had to revise vague criteria; could have been sharper in Round 1

---

## Next Document

After Phase 1 completes, team will generate **ADR-003: Phase 1 Profiling Results & Phase 2/3 Sequencing Decision**

This will contain:
- Actual profiling data (per-iteration metrics, delta sizes, iteration counts, time breakdown)
- Validation/invalidation of algorithmic hypotheses
- Workqueue ROI calculation (Amdahl's law)
- Explicit decision: Path A (parallelize) OR Path B (optimize) OR Path C (both)

---

**Consensus Process Complete**
**Status**: Ready for Phase 1 Implementation
**Owner Assignment Needed**: Phase 1 profiler
**Target Start Date**: This sprint
**Target Completion Date**: 2-3 days post-start
