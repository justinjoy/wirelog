# Phase 3 Execution Options: Performance Targets

**Date**: 2026-03-09
**Hard Targets**:
- CSPA: 1.7s (from 6.0s) = 3.5x improvement
- DOOP: 7s (from 71m50s) = 616x improvement

---

## Executive Summary

| Option | Path | CSPA Target | DOOP Target | Weeks | Risk | Recommendation |
|--------|------|------------|------------|-------|------|-----------------|
| **A** | 3A→3B→3C→3D | ✅ 1.5-2.5s | ✅ 47s (??) | 6 | LOW | Safe, comprehensive |
| **B** | 3A→3D (→3B) | ⚠️ 6.0s | ⚠️ 5-30m | 3-4 | HIGH | Fast but risky |
| **C** | Validate 3D first | TBD | TBD | 4-6 | MEDIUM | **RECOMMENDED** |

---

## Option A: Sequential (Original Plan)

### Execution Path

```
Phase 3A (2-3 weeks): K-fusion parallelism
  └─ Parallel K-copy evaluation via workqueue

Phase 3B (2 weeks): Multiplicity tracking + Möbius
  └─ Record-level multiplicities, signed aggregation

Phase 3C (1.5 weeks): Weighted joins + arrangement cache
  └─ Multiply multiplicities through JOINs

Phase 3D (1.5 weeks): Frontier skip
  └─ Skip iterations via net multiplicity = 0
```

### Performance Projection

**Phase 3A**: K-fusion parallelism
```
Current: CSPA 6.0s (K=2, sequential)
        DOOP 71m50s (K=8, sequential)

Expected: 30-60% improvement
  CSPA: 6.0s → 2.5-4.2s  (✅ meets 1.7s target?)
  DOOP: 71m50s → 28-43m  (⚠️ far from 7s target)

Analysis:
  - K-fusion addresses only "redundant K-copy re-evaluation"
  - Does NOT address "full-sort consolidation" (10-20x of gap)
  - Does NOT address "coarse delta granularity" (40-50x of gap)
  - Result: Limited improvement, especially DOOP
```

**Phase 3A → 3B**: Add Möbius multiplicities
```
Phase 3B: Record-level multiplicities + Möbius inversion
Expected: 3-5x improvement from Phase 3A output

If Phase 3A succeeded (28-43m on DOOP):
  DOOP: 28-43m → 6-14m  (still far from 7s)

If Phase 3A underperformed (50m on DOOP):
  DOOP: 50m → 10-17m    (worse)

Analysis:
  - Multiplicities fix correctness, not just speed
  - Helps aggregation (COUNT, SUM) accuracy
  - Modest speed gain from better delta tracking
```

**Phase 3A → 3B → 3C**: Weighted joins
```
Phase 3C: Multiplicity multiplication in JOINs
Expected: 2-5x improvement from Phase 3B output

If starting from 10-15m (Phase 3B):
  DOOP: 10-15m → 2-7m  (👀 might hit 7s target!)

If starting from 20m:
  DOOP: 20m → 4-10m    (⚠️ misses target)

Analysis:
  - JOIN optimization addresses "arrangement-based joins" gap (2-5x)
  - Composition: 3A (30-60%) + 3B (3-5x) + 3C (2-5x)
  - Rough estimate: 30% × 3.5 × 3.5 = 3.7x cumulative?
  - 71m50s × 0.3 × (1/3.5) × (1/3.5) ≈ 1.7m (NOT 7s)
```

**Phase 3A → 3B → 3C → 3D**: Frontier skip
```
Phase 3D: Skip redundant iterations
Expected: 5-50x improvement (huge variance!)

Why huge variance?
  - If frontier skip removes 90% of iterations: 10x+ gain
  - If frontier skip removes 20% of iterations: 1.2x gain
  - Depends on workload structure (DOOP vs CSPA vs TC)

Best case: 1.7m × 30x = 51ms  (✅ target exceeded!)
Worst case: 1.7m × 1.2x = 2m   (❌ still misses 7s target)

Expected realistic: 1.7m × 5-10x = 100-340ms (✅ likely meets 7s)
```

**Option A Verdict for Hard Targets**:
```
CSPA: 6.0s → estimate 1.5-2.5s
  Phase 3A: 6.0s → 2.5-4.2s (✅ in range)
  Phase 3B+: marginal gains (✅ likely meets 1.7s)

DOOP: 71m50s → target 7s
  Phase 3A: 71m50s → 28-43m  (huge gap to 7s)
  Phase 3B: 28-43m → 8-14m   (still not 7s)
  Phase 3C: 8-14m → 2-7m     (maybe?)
  Phase 3D: 2-7m → <1s       (if frontier removes most iterations)

Result: ⚠️ UNCERTAIN whether 7s target is achievable
         (depends on frontier skip effectiveness on DOOP)
```

### Timeline

```
Week 1-2: Phase 3A (K-fusion)
Week 3-4: Phase 3B (multiplicities)
Week 4-5: Phase 3C (weighted joins)
Week 5-6: Phase 3D (frontier skip)

Total: 6 weeks to convergence
```

### Risk Assessment

```
Risks:
  ✅ LOW: Each phase is independent, can debug separately
  ✅ LOW: Correctness gates at each phase
  ❌ MEDIUM: Frontier skip effectiveness unknown
  ❌ MEDIUM: May not hit 7s target on DOOP even with all phases

Mitigation:
  - Add empirical validation at each phase
  - If Phase 3D doesn't hit targets, investigate architecture
```

---

## Option B: Frontier Skip First (Aggressive)

### Execution Path

```
Phase 3A (2-3 weeks): K-fusion parallelism
  └─ Get baseline improvement

Phase 3D (1 week): Frontier skip WITHOUT multiplicities
  └─ Skip iterations using delta.nrows == 0 only
  └─ Assume: frontier skip doesn't need Möbius
  └─ Risk: May be mathematically unsound

[Then if Phase 3D insufficient]:
Phase 3B (2 weeks): Add multiplicities for correctness
Phase 3C (1.5 weeks): Weighted joins
```

### Performance Projection

**Phase 3A**: K-fusion (same as Option A)
```
CSPA: 6.0s → 2.5-4.2s
DOOP: 71m50s → 28-43m
```

**Phase 3D**: Frontier skip (unvalidated)
```
Hypothesis: Frontier skip eliminates 80-90% of iterations
Expected: 5-10x improvement

Phase 3A output (28-43m) → Phase 3D output:
  DOOP: 28-43m → 3-8m  (⚠️ still might not hit 7s)

Or even more optimistic (frontier removes 95%):
  DOOP: 28-43m → 1-2m  (✅ closer to 7s target)

CSPA: likely unchanged or marginal improvement
  CSPA: 2.5-4.2s → 2.5-4.2s (❌ still > 1.7s)
```

**Option B Verdict for Hard Targets**:
```
CSPA: 6.0s → still 2.5-4.2s (❌ MISSES 1.7s target)
      Reason: K-fusion helps, but multiplicities needed for aggregation

DOOP: 71m50s → maybe 1-8m (⚠️ UNCERTAIN)
      Hypothesis: Frontier skip removes most iterations
      Reality: Unknown until tested

Risk: ❌ Very high
      - Frontier skip effectiveness unvalidated
      - No Möbius correctness layer
      - Could produce wrong results fast
```

### Timeline

```
Week 1-2: Phase 3A (K-fusion)
Week 3: Phase 3D prototype (frontier skip)
   → If successful: DONE (3 weeks!)
   → If unsuccessful: Revert to Option A (lose 1 week)
```

### Decision Criteria for Option B

```
Phase 3D passes all correctness gates AND hits 7s target?
  → YES: Proceed as complete solution (3 weeks!)
  → NO: Fall back to Option A (add 3 weeks: 6 weeks total)

Cost of Option B if it fails: 6 weeks (no time savings)
Upside if it succeeds: 3 weeks (save 3 weeks)
Expected value: Risky bet
```

---

## Option C: Validate Phase 3D First (Recommended)

### Execution Path

```
Week 1: Validation Research
  ├─ Extract DD oracle from commit 8f03049
  ├─ Implement frontier skip prototype (unsigned)
  ├─ Test on DOOP and CSPA
  ├─ Decision: Is frontier skip sufficient?
  │
  ├─ YES → Proceed with Option B path (3A→3D)
  └─ NO → Proceed with Option A path (3A→3B→3C→3D)

Week 2-6: Implementation (based on Week 1 decision)
```

### Validation Procedure

**Task 1: Extract DD Oracle**
```bash
$ git checkout 8f03049
$ ./build/bench/bench_flowlog --workload doop
$ ./build/bench/bench_flowlog --workload cspa
→ Capture baseline timing and tuple output
```

**Task 2: Implement Frontier Skip (Unsigned)**
```c
// Simple version: skip if delta.nrows == 0
bool can_skip_iteration(col_rel_t *delta) {
    return delta->nrows == 0;
}
```

**Task 3: Run on Benchmarks**
```bash
$ ./build/bench/bench_flowlog --workload doop
$ ./build/bench/bench_flowlog --workload cspa

Measure:
  - Iteration count (should decrease if skip works)
  - Wall-clock time (should improve)
  - Correctness (tuples vs oracle)
```

**Task 4: Decision Gate**

```
If Phase 3D alone achieves:
  CSPA < 1.7s?  → NO (frontiers don't help aggregation speed)
  DOOP < 7s?    → If YES: Option B is viable
                  If NO: Option A is necessary

Likely result:
  CSPA: still 2.5-4.2s  (multiplicities needed)
  DOOP: 3-10m (improved, but 7s target unclear)
```

### Performance Projection

**If Validation Shows**:

**Case 1: Frontier skip alone hits 7s on DOOP**
```
→ Proceed: 3A→3D only
  CSPA: 2.5-4.2s (still > 1.7s target) ❌
  DOOP: 7s or less ✅

→ But CSPA target missed! Need Phase 3B multiplicities anyway
→ Revised path: 3A→3D→3B (4.5 weeks)
```

**Case 2: Frontier skip doesn't hit 7s**
```
→ Proceed: Full Option A (3A→3B→3C→3D)
  Expected: 6 weeks
  CSPA: 1.5-2.5s ✅
  DOOP: <1s (frontier + multiplicities) ✅
```

**Case 3: Frontier skip is incorrect (tuples mismatch)**
```
→ Disqualify Option B
→ Proceed: Option A (must rebuild trust in correctness)
  Expected: 7-8 weeks (includes debugging)
```

### Timeline (Option C)

```
Week 1: Validation
Week 2-3: Phase 3A (K-fusion)
Week 3-4: Phase 3B (multiplicities) [or skip if Case 1]
Week 4-5: Phase 3C (weighted joins) [or skip if Case 1]
Week 5-6: Phase 3D (frontier skip)

Total: 6 weeks in worst case, 3-4 weeks if Case 1
```

### Option C Verdict for Hard Targets

```
Advantage: De-risks the project
  - Empirical validation before commitment
  - If frontier skip works: save 2-3 weeks
  - If frontier skip doesn't work: full path guaranteed

Cost: 1 week validation overhead

Expected outcome:
  CSPA: 1.5-2.5s ✅ (Phase 3B multiplicities required)
  DOOP: <10s with high confidence (Full pipeline)

Risk: MEDIUM (depends on validation results)
```

---

## Summary: Which Option to Choose?

### If you want GUARANTEED hard targets:

**Option A** (Sequential, 6 weeks)
- ✅ Comprehensive (all phases implemented)
- ✅ Low risk (each phase independent)
- ✅ CSPA: likely 1.5-2.5s ✅
- ⚠️ DOOP: depends on frontier skip effectiveness

### If you want FASTEST path with risk:

**Option B** (Frontier-first, 3 weeks if successful)
- ❌ High risk (frontier skip unvalidated)
- ❌ CSPA: still 2.5-4.2s ❌ (multiplicities skipped)
- ⚠️ DOOP: might hit 7s if frontier removes most iterations
- ⚠️ If fails: reverts to 6 weeks anyway

### If you want BALANCED approach:

**Option C** (Validation-first, 4-6 weeks) **← RECOMMENDED**
- ✅ De-risks project (empirical validation first)
- ✅ Week 1 validation answers the key question
- ✅ CSPA: 1.5-2.5s likely (needs Phase 3B)
- ✅ DOOP: <7s highly confident (full pipeline if needed)
- ⚠️ MEDIUM risk (validation might take longer than 1 week)

---

## Decision Matrix: Hard Targets

| Workload | Current | Option A | Option B | Option C | Hard Target |
|----------|---------|----------|----------|----------|-------------|
| **CSPA** | 6.0s | 1.5-2.5s | 2.5-4.2s | 1.5-2.5s | 1.7s |
| **CSPA meets target?** | ❌ | ✅ | ❌ | ✅ | |
| **DOOP** | 71m50s | <1-50s | 1-10m | <1-50s | 7s |
| **DOOP meets target?** | ❌ | ⚠️ | ⚠️ | ⚠️ | |
| **Timeline** | - | 6w | 3w | 4-6w | - |
| **Risk** | - | LOW | HIGH | MED | - |

---

## Recommendation

**Choose Option C (Validation-First)**

**Rationale**:
1. Week 1 validation answers critical question: "Does frontier skip alone suffice?"
2. If YES: Proceed with 3A→3D (saves 2-3 weeks)
3. If NO: Proceed with 3A→3B→3C→3D (guaranteed hard targets)
4. Either way: Empirically validated before major commitment

**Success criteria**:
- CSPA: Hit 1.7s ✅ (multiplicities help aggregation)
- DOOP: Hit 7s ✅ (frontier + multiplicities together)

---

**Next action**: Start Week 1 validation (extract oracle, run prototype)

