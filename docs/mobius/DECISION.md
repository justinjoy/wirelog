# Execution Decision: Which Option Achieves Hard Targets?

**Date**: 2026-03-09
**Known Data**: CSPA 6.0s, DOOP 71m50s (current state)
**Hard Targets**: CSPA ≤ 1.7s, DOOP ≤ 7s

---

## The Question

Without oracle extraction (we already know current performance), which Option (A, B, or C) **will actually hit the hard targets**?

---

## Analysis: Phase-by-Phase Impact

### Current Baseline

```
CSPA: 6.0s (K=2, sequential evaluation)
DOOP: 71m50s = 4,310s (K=8, sequential evaluation)

Gap to targets:
  CSPA: 6.0s → 1.7s = 3.5x improvement needed
  DOOP: 4,310s → 7s = 615x improvement needed
```

### Phase 3A: K-fusion Parallelism

**What it does**: Parallel K-copy evaluation instead of sequential

**Expected improvement**: 30-60% (from existing analysis)

```
CSPA: 6.0s × 0.4-0.7 = 2.4-4.2s
DOOP: 4,310s × 0.4-0.7 = 1,724-2,586s (still 247-370 minutes)

Verdict on targets:
  CSPA: ⚠️ 2.4-4.2s > 1.7s target (needs more)
  DOOP: ❌ 247-370m >> 7s target (HUGE gap remains)
```

### Phase 3B: Multiplicity Tracking + Möbius Inversion

**What it does**: Record-level multiplicities, signed aggregation, Möbius delta computation

**Expected improvement**: 3-5x (multiplicities fix delta granularity)

```
Starting from Phase 3A output (best case 2.4s for CSPA):

  CSPA: 2.4s ÷ 3.5 = 0.69s  OR  2.4s ÷ 3 = 0.8s (if 3x improvement)
  CSPA: ✅ 0.69-0.8s < 1.7s ✅

Starting from Phase 3A (worst case 4.2s):
  CSPA: 4.2s ÷ 3 = 1.4s  ✅ Still meets 1.7s

For DOOP (starting from worst case 2,586s):
  DOOP: 2,586s ÷ 3 = 862s (14 minutes)
  DOOP: ❌ 14m >> 7s target

Verdict on targets:
  CSPA: ✅ LIKELY MEETS 1.7s target
  DOOP: ❌ 14m still 120x away from 7s
```

### Phase 3C: Weighted Joins + Arrangement Cache

**What it does**: Multiplicity multiplication in JOINs, per-worker caching

**Expected improvement**: 2-5x (join optimization)

```
Starting from Phase 3B (best case for DOOP: 862s):

  DOOP: 862s ÷ 2 = 431s (7 minutes)  [conservative 2x]
  DOOP: ❌ 431s >> 7s

  DOOP: 862s ÷ 5 = 172s (2.9 minutes)  [optimistic 5x]
  DOOP: ❌ 172s >> 7s

Verdict on targets:
  CSPA: ✅ STILL MEETS 1.7s
  DOOP: ❌ Even with 5x improvement, still 20-60 minutes away
```

### Phase 3D: Frontier Skip

**What it does**: Skip iterations where delta is empty

**Expected improvement**: 5-50x (HUGE variance!)

Why the huge variance?
- If frontier skip removes 90% of iterations: 10x+ gain
- If frontier skip removes 20% of iterations: 1.2x gain
- **Depends entirely on DOOP workload structure** (unknown without testing)

```
Scenario 1 (Conservative): Frontier removes 30% of iterations
  DOOP: 172s ÷ 1.3 = 132s (still 18x away from 7s) ❌

Scenario 2 (Moderate): Frontier removes 75% of iterations
  DOOP: 172s ÷ 4 = 43s (still 6x away from 7s) ❌

Scenario 3 (Optimistic): Frontier removes 95% of iterations
  DOOP: 172s ÷ 20 = 8.6s (barely misses 7s) ⚠️

Scenario 4 (Best case): Frontier removes 99% of iterations
  DOOP: 172s ÷ 100 = 1.7s ✅

Verdict on targets:
  CSPA: ✅ MEETS 1.7s
  DOOP: ❓ Depends on frontier effectiveness (Scenarios 3-4 needed)
```

---

## The Math: Can Any Phase Combination Hit 7s?

### DOOP Target Analysis

```
Starting point: 71m50s = 4,310s

Path to 7s:
  4,310s × 0.5 × 0.25 × 0.25 × 0.1 = 1.7s (achievable!)

  Where:
    ×0.5  = K-fusion (Phase 3A): 30-50% improvement
    ×0.25 = Multiplicities (Phase 3B): 4x improvement
    ×0.25 = Weighted joins (Phase 3C): 4x improvement
    ×0.1  = Frontier skip (Phase 3D): 10x improvement

  Cumulative: 0.5 × 0.25 × 0.25 × 0.1 = 0.0031 ≈ 615x improvement

Conclusion:
  - Theoretically possible (615x total needed)
  - Phase improvements must compose multiplicatively
  - Frontier skip MUST deliver 10x+ (not just 2-5x)
```

### The Problem

**DOOP's structure prevents most iteration skipping**:

From the three articles and Differential Dataflow semantics:
- DOOP = class hierarchy analysis, 136 rules, 8-way joins
- Each iteration derives NEW facts (doesn't saturate quickly)
- Frontier skip only helps when Δ(i) = 0 (no new facts)

**DOOP characteristics**:
- Many rules with low selectivity (most derive something)
- Deep dependency chains (rule output feeds into other rules)
- Result: Even after multiplicities + weighted joins, many iterations still produce new facts

**Reality check**:
- DD (Timely) achieved 47s on DOOP via distributed multi-worker + incremental
- Pure single-threaded semi-naive (even with frontier) might not reach 7s
- The 91x gap might require architectural changes beyond multiplicities + frontier

---

## Honest Assessment by Option

### Option A: Sequential (3A → 3B → 3C → 3D)

**CSPA**:
- Phase 3A: 6.0s → 2.4-4.2s
- Phase 3B: 2.4-4.2s → 0.7-1.4s
- Result: ✅ **MEETS 1.7s target**

**DOOP**:
- Phase 3A: 71m50s → 28-43m
- Phase 3B: 28-43m → 6-14m
- Phase 3C: 6-14m → 1.5-7m
- Phase 3D: 1.5-7m → depends on frontier effectiveness
  - Need frontier to remove 80%+ of remaining iterations
  - If frontier removes only 60%: 2.4-4.5m (still misses 7s)
  - If frontier removes 95%: 75-400ms (hits 7s!) ✅
- Result: ⚠️ **UNCERTAIN (depends on DOOP iteration structure)**

**Verdict**: A **SAFE BET on CSPA, RISKY on DOOP**

---

### Option B: Frontier-First (3A → 3D → 3B/3C if needed)

**CSPA**:
- Phase 3A: 6.0s → 2.4-4.2s
- Phase 3D: 2.4-4.2s → likely unchanged (frontier skip doesn't help aggregation)
- Result: ❌ **MISSES 1.7s target** (no Phase 3B multiplicities)

**DOOP**:
- Phase 3A: 71m50s → 28-43m
- Phase 3D: 28-43m → depends on frontier (same as Option A)
- If frontier works: ⚠️ maybe 1.5-7m
- If frontier doesn't work: ❌ still 28-43m
- Result: ⚠️ **UNCERTAIN, and CSPA target definitely missed**

**Verdict**: B **FAILS CSPA TARGET GUARANTEED, DOOP uncertain**

---

### Option C: Validate First (1 week validation → decide A or B)

**If validation shows frontier skip works (removes 80%+ iterations)**:
- Proceed: 3A → 3D (saves time, but CSPA still missed)
- Result: ❌ CSPA not met, DOOP maybe met

**If validation shows frontier skip doesn't work well**:
- Proceed: Full 3A → 3B → 3C → 3D
- Result: ✅ CSPA likely met, DOOP uncertain

**Verdict**: C **DELAYS DECISION by 1 week, outcome same as A or B**

---

## The Uncomfortable Truth

### Can DOOP Really Hit 7s?

**What we know**:
- DD + Timely hit 47s (but distributed multi-worker)
- Single-threaded semi-naive in C has fundamental limits
- Frontier skip helps when Δ(i) = 0, but DOOP produces facts almost every iteration

**Mathematical gap**:
```
Current: 71m50s
Target: 7s
Gap: 615x

Phase improvements (realistic):
  3A: 1.5x (K-fusion on 8-way joins)
  3B: 3x (multiplicities for better delta tracking)
  3C: 2x (weighted joins optimization)
  3D: 5x (frontier skip at best)

Cumulative: 1.5 × 3 × 2 × 5 = 45x

Result: 71m50s ÷ 45 = 96s (not 7s)
```

**Missing 13x of improvement** — likely requires:
- Full incremental computation (currently missing)
- Or multi-worker execution (Phase 2B, not Phase 3)
- Or architectural changes beyond current scope

---

## Recommendation: REALISTIC TARGETS

### If you must hit both targets:

**CSPA 1.7s**: ✅ **Achievable** (Option A: Phases 3A + 3B sufficient)

**DOOP 7s**: ❌ **Unlikely with current architecture**
- Option A might hit 30-400s range
- Depending on frontier skip effectiveness
- But 7s very unlikely without:
  - Full multi-worker (Phase 2B)
  - Distributed execution
  - Or different evaluation strategy

### Revised Hard Targets (Realistic)

Based on phase analysis:

```
CSPA: 6.0s → 1.5s (3.5x improvement) ✅ ACHIEVABLE via 3A+3B
DOOP: 71m50s → 60-300s (12-70x improvement) ✅ ACHIEVABLE via all phases
      (But 7s specifically: ❌ VERY UNLIKELY)
```

---

## Decision: Which Option to Execute?

### The Only Viable Option: **Option A (Sequential)**

**Why**:
1. CSPA target (1.7s): ✅ Will be met by 3A+3B
2. DOOP target (7s): ⚠️ Uncertain, depends on frontier effectiveness
   - But worth trying (at minimum hits 60-300s range)
3. CSPA is critical (aggregation correctness)
   - Phase 3B multiplicities **required**
   - Option B skips this, so ❌

**Not Option B**:
- CSPA target is definitely missed (no multiplicities)
- DOOP benefit is marginal (frontier skip without foundational layers)
- Wastes 1 week on wrong direction

**Not Option C**:
- Validation (oracle extraction) doesn't change the decision
- We already know current state
- Option A is the clear path regardless

---

## Final Verdict

**Execute Option A (Sequential): 3A → 3B → 3C → 3D**

**Confidence**:
- CSPA 1.7s target: ✅ 95% confident will be met
- DOOP 7s target: ❌ 20% confident (too aggressive)
- DOOP 60-300s range: ✅ 90% confident (realistic)

**Timeline**: 6 weeks

**Next action**: Start Phase 3A immediately (K-fusion parallelism)

