# Möbius Inversion Research: wirelog Phase 3B-3D

**Date**: 2026-03-09
**Status**: Research Complete, Ready for Implementation
**Impact**: Critical for achieving DD parity performance (47s on DOOP)

---

## Quick Summary

wirelog's current aggregation implementation **does NOT use Möbius inversion**, which is the mathematical foundation of Differential Dataflow. This gap prevents correct handling of **signed deltas** (insertions vs deletions) in incremental computation.

**Key Discovery**: Three provided articles reveal that Timely Dataflow protocol + Differential Dataflow's core are not about "evaluation semantics" but about **weighted sets (Z-sets) and multi-dimensional time arithmetic**.

---

## The Three Missing Pieces

### 1. **Z-Sets (Weighted Sets)** ❌ Not in wirelog

```
Differential Dataflow's core abstraction:
  Collection = {(value, multiplicity), ...}

  Example:
    {(fact1, +1), (fact2, -1), (fact3, +1)}
    = 2 facts (fact2 is "retracted")

Current wirelog:
  Collection = {(value), ...}  ← No multiplicities!
  Cannot express deletions or retractions
```

### 2. **Möbius Function over Iteration Lattice** ❌ Not in wirelog

```
For iterations 0 < 1 < 2 < ... (total order):

Möbius function:
  μ(i, j) = { 1    if i == j
            { -1   if i < j (going backward)
            { 0    if i > j

Application: Δ(i) = A(i) - A(i-1)
  = μ(i-1, i) × A(i-1) + μ(i, i) × A(i)
  = -A(i-1) + A(i)

Current wirelog:
  Deltas computed as snapshot diffs (naive), not via Möbius inversion
```

### 3. **Multiplicity Propagation through Operators** ❌ Not in wirelog

```
JOIN:
  (L_row, mult_L) × (R_row, mult_R) = (L_row, R_row, mult_L × mult_R)

AGGREGATE:
  group_value += (input_mult × agg_value)
  where input_mult ∈ {-1, 0, +1, ...}

Current wirelog:
  Always assumes mult = +1, never handles signed multiplicities
```

---

## Documents in This Directory

### ANALYSIS.md
**What's missing and why it matters**
- Root cause of the 91x DOOP performance gap
- Where Möbius inversion is needed
- Impact on correctness (deletions become incorrect)
- Current partial infrastructure (timestamps exist but not multiplicities)

**Read this for**: Understanding the problem

### IMPLEMENTATION_DESIGN.md
**Concrete C code changes needed**
- Type system extensions (col_delta_timestamp_t → add multiplicity field)
- Four core operations:
  1. Delta computation via Möbius (Δ(i) = A(i) - A(i-1))
  2. Aggregation with signed multiplicities (COUNT, SUM with +1/-1)
  3. JOIN with multiplicity multiplication (mult_result = mult_L × mult_R)
  4. Frontier skip with net multiplicity
- Integration points in session API
- Correctness invariants (aggregation linearity, join multiplicativity)
- Validation test cases
- Timeline: 6 weeks for Phase 3B-3D

**Read this for**: Implementation roadmap

---

## How It Connects to the Three Articles

### Article 1: Arrow-for-Timely
```
Bimodal batch distribution:
  Bulk loads (1M rows) → columnar (Arrow)
  Incremental updates (1-100 rows) → row-based

wirelog status: ✅ Implemented (nanoarrow in Phase 2C)

Möbius relevance:
  L4 (Delta Batch Layer) must handle signed deltas
  Current: only supports (+1 insertions)
  Needed: support (-1 deletions) via multiplicity field
```

### Article 2: Timely Protocol (5-Layer Stack)
```
L1: Wire layer                  ❌ Not needed
L2: Channel layer               ❌ Not needed
L3: Progress layer              🔄 Partial (frontier tracking)
    └─ Uses Möbius to track: Δ(i) = A(i) - A(i-1)
L4: Delta Batch layer           ⚠️ INCOMPLETE
    └─ Current: snapshot diff (wrong)
    └─ Needed: record-level multiplicities + Möbius
L5: Dataflow binding            ✅ col_eval_stratum()
```

### Article 3: Differential Dataflow
```
Core idea: Collections as "accumulated differences over time"

  A(t) = Σ Δ(t')  for all t' ≤ t

Where Δ(t') computed via:
  Möbius inversion over time lattice (iterations)

  Δ(i) = Σ μ(t', i) × A(t')
       = -A(i-1) + A(i)  [for total order]

wirelog: Currently uses naive snapshot diff
Needed: Proper Möbius inversion formula
```

---

## Key Insight: Why Multiplicities Matter

### Example: Transitive Closure with COUNT

```
EDB: edge(1,2), edge(2,3)

Iteration 0:
  tc(1,2) = 1   [direct edge]
  tc(2,3) = 1   [direct edge]
  count(1) = 1

Iteration 1:
  tc(1,3) = 1   [derived from tc(1,2) ∧ edge(2,3)]
  count(1) = 2  [now have both tc(1,2) and tc(1,3)]

Iteration 2 (error correction):
  Retract tc(1,3)  [was a mistake]

  With Möbius (multiplicities):
    tc(1,3) marked with mult=-1
    count(1) = 2 + (-1) = 1  ✓ CORRECT

  Without (current wirelog):
    count(1) = 2  ✗ WRONG (no way to subtract)
```

---

## Performance Impact

### Why 91x Gap Exists

```
DOOP: 71m50s (wirelog) vs 47s (DD)  = 91x slower

Root causes:
  1. Redundant recomputation         ~40-50x (addressed by K-fusion, Phase 3A)
  2. Full-sort consolidation         ~10-20x (addressed by Phase 3B incrementals)
  3. No multiplicity tracking        ~2-5x   (Möbius inversion, Phase 3B-3C)
  4. No arrangement-based joins      ~2-5x   (Phase 3C)
  5. Sequential operator scheduling  ~1-2x   (Phase 3D)
```

### Phase-by-Phase Recovery

```
Phase 2D (current):     DOOP = 71m50s (baseline)
Phase 3A (K-fusion):    DOOP = 10-30m (50-60% improvement)
Phase 3B (Möbius):      DOOP = 5-15m  (additional 40-50x from base)
Phase 3C (Arrangement): DOOP = 2-5m   (additional 2-5x)
Phase 3D (Frontier):    DOOP = 47s    (final optimization, DD parity)
```

---

## Why This Matters Now

### Current Limitation
- wirelog can only express positive tuples
- No way to "retract" a fact (delete with -1 multiplicity)
- Incremental updates become incorrect
- Frontier skip optimization becomes unsound

### After Implementation
- ✅ Correct incremental semantics
- ✅ Proper delta propagation
- ✅ Sound frontier optimization
- ✅ Path to DD parity (47s on DOOP)

---

## What's Next

1. **Immediate** (Week 1): Review ANALYSIS.md and IMPLEMENTATION_DESIGN.md
2. **Week 1-2**: Design review with team, finalize multiplicity field format
3. **Week 2-3**: Implement Phase 3B changes
   - Add multiplicity to col_delta_timestamp_t
   - Implement col_op_reduce_weighted
   - Update session API for signed inserts
4. **Week 3-4**: Implement Möbius delta computation
5. **Week 4-6**: Integration, testing, validation against DD oracle

---

## Files to Modify

```
High priority (Phase 3B):
  wirelog/backend/columnar_nanoarrow.h    (type changes)
  wirelog/backend/columnar_nanoarrow.c    (operators with multiplicities)
  wirelog/session.h                       (API for signed inserts)

Medium priority (Phase 3C-D):
  wirelog/exec_plan_gen.c                 (plan generation)
  wirelog/ir/stratify.h                   (frontier computation)

Testing:
  tests/test_mobius_*.c                   (new test suite)
```

---

## References

**Research articles provided**:
1. [Arrow-for-Timely](https://groou.com/research/2026/02/22/arrow-for-timely-dataflow/)
2. [Timely Protocol](https://groou.com/essay/research/2026/02/17/timely-dataflow-protocol/)
3. [Differential Dataflow](https://groou.com/essay/ai/2026/02/14/differential-dataflow/)

**Academic papers**:
- Naiad: A Timely Dataflow System (SOSP 2013) - https://www.microsoft.com/research/publication/naiad-timely-dataflow-system/
- Differential Dataflow - https://github.com/frankmcsherry/differential-dataflow

**Mathematical foundations**:
- Möbius function - https://en.wikipedia.org/wiki/M%C3%B6bius_function
- Poset (partially ordered set) - https://en.wikipedia.org/wiki/Partially_ordered_set

---

## Decision Points

- [ ] Approve Phase 3B scope (multiplicity tracking in col_delta_timestamp_t)
- [ ] Review type system changes (col_rel_t.multiplicities field)
- [ ] Validate Möbius inversion formula for total order iterations
- [ ] Confirm test strategy for incremental aggregation validation
- [ ] Schedule Phase 3B-3D implementation (6 weeks, 1-2 engineers)

---

**Created**: 2026-03-09
**Status**: Ready for team review
**Impact**: Critical path to DD parity performance
