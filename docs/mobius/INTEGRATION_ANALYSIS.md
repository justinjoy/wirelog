# Integration Analysis: Möbius Inversion, Weighted Joins, and Frontier Skip

**Date**: 2026-03-09
**Status**: RALPLAN Consensus Planning - Planner Draft
**Critical Question**: Is Möbius inversion truly the key to DD parity? What are the exact dependencies between Phase 3B, 3C, and 3D?

---

## RALPLAN-DR Summary (Principles, Drivers, Options)

### Principles (5)

**P1: Correctness Over Optimization**
- Weighted sets (Z-sets) are foundational to Differential Dataflow's correctness
- Any phase that omits multiplicity tracking is mathematically incomplete
- Non-negotiable: signed deltas must propagate correctly through all operators

**P2: Dependency-Aware Phasing**
- Phases must be sequenced by dependency, not by convenience
- If Phase 3D (frontier skip) depends on Phase 3C (weighted joins), don't parallelize them
- Conversely, if frontier skip works without weighted joins, we can reorder for faster ROI

**P3: Empirical Validation Required**
- "DD parity" claim requires proof against DD oracle on all 15 workloads
- Theoretical 91x gap analysis may not predict actual Phase-by-phase recovery
- Each phase must deliver measurable improvement; phases that miss targets block subsequent phases

**P4: Protocol Fidelity to Three Articles**
- Arrow-for-Timely: L4 Delta Batch layer with bimodal batches
- Timely Protocol: L3 progress tracking REQUIRES Möbius for iteration boundaries
- Differential Dataflow: Z-sets are the abstraction; Möbius is the math
- Implementation must honor these three articles, not deviate

**P5: Bounded Complexity**
- Phase 3B budget: 1,500 LOC (col_delta_timestamp_t + col_op_reduce_weighted)
- Phase 3C budget: 1,000 LOC (col_op_join_weighted + arrangement cache)
- Phase 3D budget: 500 LOC (frontier computation + skip logic)
- Total 6 weeks for 3,000 LOC is realistic; 5,000+ LOC is not feasible

---

### Decision Drivers (Top 3)

**Driver 1: The "Möbius is the key" hypothesis**

From the three articles, Möbius inversion appears critical because:
- **Arrow-for-Timely**: L4 must handle signed deltas (multiplicities)
- **Timely Protocol**: L3 progress requires Möbius over iteration lattice
- **Differential Dataflow**: Z-sets + Möbius = correct incremental semantics

**BUT**: Current wirelog doesn't do signed deltas at all. The question is:
- Can frontier skip work with unsigned multiplicities? (all weights = +1)
- Or does frontier skip REQUIRE signed multiplicities to be sound?

**Critical insight**: Frontier skip is an OPTIMIZATION. Unsigned deltas might work, just less efficiently.

---

**Driver 2: Dependency inversion risk**

If we implement phases in the assumed order (3A → 3B → 3C → 3D):
- Phase 3B adds multiplicity tracking (cost: time, memory, complexity)
- Phase 3C uses multiplicities in JOIN (multiplicity × multiplicity)
- Phase 3D frontier skip needs net multiplicity = 0 check

**RISK**: What if Phase 3D (frontier skip) doesn't actually need Phase 3B+3C?

Scenario:
- Frontier skip could use **relation count** instead of **net multiplicity**
- Then Phase 3B is optional (only for correctness, not for frontier)
- Would change priority: Phase 3D first (1-2 weeks, high ROI), Phase 3B later (optional)

This changes the 6-week plan entirely.

---

**Driver 3: The "DOOP is special" observation**

91x gap for DOOP, but:
- CSPA: 6.0s (K=2, low K) → Phase 3A alone might suffice (3-4x improvement to 1.5-2s)
- DOOP: 71m50s (K=8, high K) → Might need all phases
- TC/CC: <0.1s → No improvement needed

**DOOP's structure**:
- 136 rules, 8-way joins, class hierarchy analysis
- High join fan-out (multiplicities explode)
- If frontier skip can prune iterations, 91x gap collapses

**Question**: Does DOOP's 91x gap come from:
- A) Missing Möbius + multiplicities (requires Phase 3B-3C)
- B) Missing frontier skip alone (requires Phase 3D only)
- C) Both, but in different ratios

---

### Viable Options (3)

#### Option A: Sequential Phasing (Original Plan)

**Sequence**: 3A → 3B → 3C → 3D

```
Phase 3A (2-3 weeks): K-fusion parallelism
  - Complete workqueue dispatch
  - Expected: 30-60% improvement (DOOP: 71m → 30-40m)

Phase 3B (2 weeks): Multiplicity tracking + Möbius
  - Add int64_t multiplicity to col_delta_timestamp_t
  - Implement signed aggregation (COUNT += ±1)
  - Expected: 3-5x improvement (DOOP: 30-40m → 10-15m)
  - Risk: Low (infrastructure exists, clear semantics)

Phase 3C (1.5 weeks): Weighted joins
  - Implement col_op_join_weighted (mult = mult_L × mult_R)
  - Expected: 2-5x improvement (DOOP: 10-15m → 2-5m)
  - Dependency: REQUIRES Phase 3B (multiplicities must exist)

Phase 3D (1.5 weeks): Frontier skip
  - Check net_multiplicity == 0 to skip iterations
  - Expected: 5-50x improvement (DOOP: 2-5m → 47s)
  - Dependency: Could work with or without Phase 3C?
  - Risk: HIGH (unclear if multiplicities are truly needed for skip)

Total: 6 weeks, all features
ROI curve: Smooth, every phase adds value
Risk: Dependency explosion if Phase 3B blocks Phase 3C or 3D
```

**Pros**:
- ✅ Sequential: Phases validated independently
- ✅ Correctness first: All multiplicities in place before using them
- ✅ Matches original analysis

**Cons**:
- ❌ Long critical path (6 weeks before frontier skip)
- ❌ Phase 3B might be unnecessary if frontier skip works without it
- ❌ High complexity budget (3,000 LOC)

---

#### Option B: Frontier Skip First (Aggressive)

**Sequence**: 3A → 3D → 3B → 3C (or 3A → 3D → 3B || 3C)

```
Phase 3A (2-3 weeks): K-fusion
  - Expected: 30-60% (DOOP: 71m → 30-40m)

Phase 3D (1 week): Frontier skip WITHOUT multiplicities
  - Implement: if (relation_count == 0) skip_iteration()
  - Expected: 10-50x improvement (DOOP: 30m → 1-3m, or 47s if lucky)
  - Assumption: Iteration skip doesn't require signed deltas
  - Risk: VERY HIGH (unvalidated)

Phase 3B (2 weeks): Multiplicities (if Phase 3D is insufficient)
  - Add Möbius correctness layer
  - Expected: 3-5x (DOOP: 1-3m → 20-60s)

Phase 3C (1.5 weeks): Weighted joins (for correctness, not speed)
  - Ensure multiplicities propagate correctly through JOINs

Total: 6-7 weeks if all needed, or 3 weeks if Phase 3D alone suffices
ROI curve: Aggressive first, then correctness
Risk: Phase 3D might be unsound; requires oracle validation
```

**Pros**:
- ✅ Fastest to frontier skip (1 week vs 5 weeks)
- ✅ If Phase 3D alone hits 47s target, saves 2 weeks
- ✅ Faster ROI on DOOP

**Cons**:
- ❌ HIGH RISK: frontier skip might be mathematically unsound without Möbius
- ❌ Phase 3D without multiplicities = guesswork
- ❌ Violates P4 (Protocol Fidelity): Timely Protocol L3 REQUIRES Möbius
- ❌ Could waste 1 week on broken implementation

---

#### Option C: Hybrid - Validate Phase 3D Requirements First (Recommended)

**Pre-Phase**: Research & Validation (1 week)

```
1. Analyze Timely Protocol L3 (frontier tracking)
   - Read Naiad paper on pointstamps
   - Confirm: Does frontier skip require multiplicities in theory?

2. Prototype Phase 3D WITHOUT multiplicities
   - Implement simple: if (delta_rel->nrows == 0) skip
   - Run on CSPA, TC, DOOP
   - Measure actual improvement vs predicted

3. Analyze gap to DD oracle
   - If Phase 3D-only hits 47s: multiplicities are optional (fast-path)
   - If Phase 3D-only reaches 5m: multiplicities are essential

4. Decision point:
   a) Phase 3D sufficient → Proceed with Option B (3A → 3D → finish)
   b) Phase 3D insufficient → Proceed with Option A (3A → 3B → 3C → 3D)
   c) Gap still large → Architecture issue exists (don't proceed)
```

**Then**: Implement based on validation results

**Pros**:
- ✅ Reduces risk (empirical validation before commitment)
- ✅ Might discover Phase 3D is simpler than thought
- ✅ Clarifies true dependency graph
- ✅ Honors P3 (Empirical Validation)

**Cons**:
- ❌ Delays Phase 3A by 1 week (research overhead)
- ⚠️ Might reveal Option A is the only viable path (no time savings)

---

## Critical Analysis: What the Three Articles Tell Us

### 1. Arrow-for-Timely (Bimodal Batch Distribution)

```
L4 Delta Batch Layer:
  - Bulk loads (1M rows): columnar (Arrow) efficient
  - Incremental updates (1-100 rows): row-based efficient
  - Conversion at compaction boundary

wirelog today:
  ✅ Uses nanoarrow for bulk
  ⚠️ But DOESN'T use multiplicities for incremental

Article says: L4 handles both efficiently.
  → Requires: record-level weights (multiplicities)
  → For: accurate delta tracking when batches are small

Implication:
  If frontier skip looks at BATCH SIZE (L4 concept):
  → No multiplicities needed
  → Phase 3D can work independently

  If frontier skip looks at MULTIPLICITY (Z-set concept):
  → Multiplicities essential
  → Phase 3B prerequisite
```

---

### 2. Timely Protocol (5-Layer Stack)

```
L3 Progress Layer (Frontier Tracking):

The article describes:
  "frontier: per-operator lower bound on future timestamps"

Mathematical foundation (Naiad paper):
  Frontier = min timestamp that could still produce facts
  Computed via: Möbius inversion over poset of (epoch, iteration)

For wirelog's total-order iterations:
  Frontier(i) = min iteration where new facts are still possible
  Skip rule: if iter > frontier(i), skip stratum i

Implementation WITHOUT multiplicities:
  frontier(i) = "did this stratum produce output at iteration i-1?"
  if (delta[i-1].nrows == 0) frontier(i) = i-1, skip iteration i

Implementation WITH multiplicities:
  frontier(i) = "is net multiplicity of stratum i zero?"
  if (sum(multiplicities[i-1]) == 0) frontier(i) = i-1, skip iteration i

Question: Are these equivalent?
  MAYBE. Unsigned deltas (nrows > 0) ≈ signed net multiplicity != 0
  BUT: Race condition risk if multiplicities cancel (e.g., +5, -5)
       In concurrent setting, could miss skipping opportunity
  AND: Doesn't match Timely Protocol spec (L3 uses frontier messages, not row counts)
```

---

### 3. Differential Dataflow (Z-Sets + Möbius)

```
Core claim: "Collections = accumulated differences over time"

  A(t) = Σ Δ(t')  for all t' ≤ t

where Δ computed via Möbius:

  Δ(i) = Σ μ(j, i) × A(j)  for all j ≤ i

For total order:
  μ(j, i) = { 1 if j == i, -1 if j < i, 0 if j > i }

  Δ(i) = A(i) - A(i-1)

Meaning:
  New facts at iteration i = current facts minus facts from previous iteration

Critical insight:
  If A(i) = A(i-1) [no new facts]
  Then Δ(i) = 0
  Then nothing to propagate
  Then can skip iteration i+1

This is EXACTLY what frontier skip does!

Connection to multiplicities:
  In a Z-set, Δ(i) is computed as:
    Δ(i) = {(row, +1) for new rows} ∪ {(row, -1) for retracted rows}

  If using unsigned relations (no multiplicities):
    Δ(i) = new rows only (missing retracted rows)
    → Mathematically incomplete
    → Correctness issue on incremental updates

But for FRONTIER SKIP:
  We only care: "Is Δ(i) empty?"
  Empty check: nrows == 0 (unsigned)  OR  sum(mults) == 0 (signed)
  Both work!
```

---

## Dependency Graph Analysis

### If Multiplicities Are Used for Frontier Skip

```
Phase 3B: Multiplicities
  ↓ (prerequisite)
Phase 3C: Weighted Joins
  ↓ (prerequisite)
Phase 3D: Frontier Skip
  ↓ (depends on net multiplicity == 0)

Critical path: 3A → 3B → 3C → 3D (6 weeks)
Risk: HIGH (long path, many dependencies)
```

### If Multiplicities Are NOT Used for Frontier Skip

```
Phase 3A: K-fusion
  ├→ Phase 3D: Frontier Skip (independent)
  │  ↓
  └→ Phase 3B: Multiplicities (optional, for correctness only)
       ↓
     Phase 3C: Weighted Joins (correctness layer)

Critical path: 3A → 3D (3-4 weeks) OR 3A → 3B → 3C → 3D (6 weeks)
Risk: MEDIUM (two viable paths)
```

### Key Question to Answer

**Q**: In Timely Protocol L3, when computing frontier (can we skip iteration i?), do we need:

A) Just check: did the previous iteration produce any output?
   - If yes: cannot skip
   - If no: can skip (frontier advanced)
   - Implementation: `if (delta[i-1].nrows == 0) skip`
   - Multiplicities needed: ❌ NO

B) Or do we need signed multiplicities?
   - Check: is the NET multiplicity of previous iteration zero?
   - If yes: can skip (all insertions and deletions canceled)
   - If no: cannot skip
   - Implementation: `if (sum(multiplicities[i-1]) == 0) skip`
   - Multiplicities needed: ✅ YES

**Answer from three articles**:
- Arrow-for-Timely: Silent on this (focuses on L4)
- Timely Protocol: Says frontier is computed via progress messages (vague on mechanism)
- Differential Dataflow: Says Δ(i) = A(i) - A(i-1) (unsigned comparison sufficient)

**Conclusion**: Unsigned deltas (nrows check) are likely sufficient for frontier skip.
Multiplicities are needed for CORRECTNESS of aggregation, not for OPTIMIZATION of frontier.

---

## The "Möbius Inversion" Central Question

### Original Claim
"Möbius inversion is the KEY to DD parity"

### Refined Understanding

Möbius inversion is key for **correctness**, not necessarily for **performance**:

**For Correctness** (Phase 3B-3C):
- Aggregating signed deltas: COUNT with +1/-1 multiplicities
- Propagating multiplicity through JOINs: mult_result = mult_L × mult_R
- Both require Möbius semantics to be sound

**For Performance** (Phase 3D):
- Frontier skip (can we skip this iteration?) can use nrows check
- Doesn't strictly require signed multiplicities
- But Timely Protocol L3 describes progress via multiplicities

**Reconciliation**:
Implement Möbius + multiplicities (Phase 3B-C) for correctness.
Phase 3D frontier skip will naturally benefit.
But Phase 3D might work WITHOUT Phase 3B, albeit incorrectly on certain workloads.

---

## Recommended Path Forward: Option C (Validation-First)

### Week 1: Validation Research

**Task 1**: Understand Timely Protocol L3 in detail
- Read Naiad paper sections on progress tracking
- Answer: Is frontier computed from row counts or multiplicities?
- Output: Architectural decision matrix

**Task 2**: Prototype frontier skip WITHOUT multiplicities
```c
// Simple version: skip if previous iteration was empty
col_can_skip_iteration_simple(col_rel_t *delta) {
    return delta->nrows == 0;  // No new facts = can skip
}
```
- Run on 3-5 key benchmarks (CSPA, TC, DOOP)
- Measure actual improvement
- Compare to predicted 5-50x

**Task 3**: Prototype frontier skip WITH multiplicities (Phase 3B prerequisite)
```c
// Möbius version: skip if net multiplicity is zero
col_can_skip_iteration_mobius(col_rel_t *delta) {
    int64_t net = 0;
    for (size_t i = 0; i < delta->nrows; i++)
        net += delta->multiplicities[i];
    return net == 0;
}
```
- Run same benchmarks
- Compare to simple version
- Determine if multiplicities add correctness value

**Task 4**: Decision point
- If simple frontier skip (unsigned) hits 47s on DOOP: proceed with Option B
- If multiplicities needed for correctness: proceed with Option A
- If gap remains: investigate deeper (architecture issue)

---

### Decision Matrix (After Validation)

| Scenario | Frontier Skip Impact | Multiplicity Impact | Recommendation |
|----------|---------------------|-------------------|-----------------|
| Phase 3D alone → 47s | HIGH (solves gap) | LOW (bonus) | Option B: 3A→3D→finish |
| Phase 3D → 5m, Phase 3B → 47s | MEDIUM | HIGH (essential) | Option A: 3A→3B→3C→3D |
| Phase 3D → 5m, Phase 3B → 2m | MEDIUM | MEDIUM | Option A: parallel 3B+3C, then 3D |
| Phase 3D+3B → 5m, larger gap | LOW | MEDIUM | Investigate further |

---

## Next Steps (Awaiting Architect & Critic Review)

1. ✅ Planner (this draft): Identified critical unknowns
   - Is Phase 3D independent of Phase 3B-3C?
   - Do multiplicities help frontier skip, or are they separate concerns?

2. ⏳ Architect (next): Structural soundness
   - Are the three articles correctly interpreted?
   - Is the dependency analysis valid?
   - Any architectural blind spots?

3. ⏳ Critic (next): Quality and testability
   - Are decision drivers clear and complete?
   - Can we validate the hypotheses in 1 week?
   - What would prove/disprove each option?

---

## Provisional Timeline (Pending Validation)

```
Week 1 (Research):
  - Validation research (Tasks 1-4)
  - Decision: Paths 3A→3D (fast) vs 3A→3B→3C→3D (comprehensive)

If Path 3A→3D chosen:
  Week 2-3: Phase 3A (K-fusion)
  Week 4: Phase 3D (frontier skip)
  Total: 4 weeks to frontier optimization

If Path 3A→3B→3C→3D chosen:
  Week 2-3: Phase 3A (K-fusion)
  Week 3-4: Phase 3B (multiplicities)
  Week 4-5: Phase 3C (weighted joins)
  Week 5-6: Phase 3D (frontier skip)
  Total: 6 weeks to full DD parity
```

---

## References

- **Arrow-for-Timely**: https://groou.com/research/2026/02/22/arrow-for-timely-dataflow/ (L4 Delta Batch with bimodal batches)
- **Timely Protocol**: https://groou.com/essay/research/2026/02/17/timely-dataflow-protocol/ (L3 Progress via frontier)
- **Differential Dataflow**: https://groou.com/essay/ai/2026/02/14/differential-dataflow/ (Z-sets + Möbius)
- **Naiad Paper**: SOSP 2013 - Section 3.2 (Progress Tracking)
- **Previous Analysis**: `/docs/mobius/ANALYSIS.md`, `IMPLEMENTATION_DESIGN.md`

---

**Status**: Awaiting Architect and Critic review

**Awaiting**:
- ✅ Planner (COMPLETE): Identified critical hypotheses
- ⏳ **Architect**: Validate dependency analysis
- ⏳ **Critic**: Evaluate decision quality and testability
