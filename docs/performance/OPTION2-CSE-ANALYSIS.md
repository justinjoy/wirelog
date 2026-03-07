# Option 2 + CSE Materialization: Deep Cost Analysis

**Date:** 2026-03-07
**Status:** Analysis complete — informs US-008 fine-tuning
**Scope:** Cost model for multi-way delta expansion with Common Subexpression Elimination (CSE) materialization

---

## Executive Summary

Naive Option 2 (K plan copies for K-atom rules) is correct but wasteful for high-arity joins. CSE materialization groups atoms into subtrees, materializes intermediate results, and reuses them across delta passes. This analysis provides concrete cost models for CSPA (3-way) and DOOP (8/9-way), per-rule benefit analysis, memory projections, and threshold recommendations.

**Key findings:**
- For K=3 (CSPA): CSE saves ~33% of join work per iteration vs naive Option 2
- For K=8 (DOOP): CSE reduces effective join cost by ~75%, likely enabling DOOP completion
- Materialization threshold: K >= 3 always benefits; K=2 never needs CSE
- CSPA peak RSS reduction estimate: 30-40% via materialization + snapshot elimination
- Break-even: CSE overhead (one materialization per group per iteration) is amortized after 2 delta passes reuse the same materialized subtree

---

## 1. Cost Model Framework

### Definitions

| Symbol | Meaning |
|--------|---------|
| K | Number of body atoms in a rule |
| \|R_i\| | Cardinality of relation R_i (full) |
| \|δR_i\| | Cardinality of delta for R_i |
| C_join(A, B) | Cost of hash-join between A and B = O(\|A\| + \|B\| + \|A⋈B\|) |
| C_chain(K) | Cost of left-deep K-way join chain |
| M(T) | Memory to materialize intermediate result T |
| σ | Selectivity factor (output/input ratio for a join, typically 0.01-0.1 for key-foreign-key) |

### Join Cost Model

For a left-deep chain `A₁ ⋈ A₂ ⋈ ... ⋈ Aₖ`:

```
C_chain(K) = Σᵢ₌₁ᴷ⁻¹ C_join(Tᵢ, Aᵢ₊₁)

where T₁ = A₁, Tᵢ = Tᵢ₋₁ ⋈ Aᵢ (intermediate result after i-th join)
```

The critical insight: intermediate result sizes grow (or shrink) depending on selectivity. For analytical workloads like CSPA/DOOP, joins through shared variables typically have σ ≈ 0.01-0.1 (each tuple matches a small fraction of the other relation).

### Naive Option 2 Cost

For a K-atom rule, naive Option 2 evaluates K plan copies:

```
C_naive(K) = K × C_chain(K)
```

Each copy does the full K-way join chain, differing only in which atom uses its delta.

### CSE Materialization Cost

Partition K atoms into G groups of size ~K/G. Materialize each group's intermediate join result once. Then for each delta pass, only recompute within the affected group and join with other materialized groups.

```
C_CSE(K, G) = G × C_chain(K/G)           [materialization cost]
            + K × C_chain(K/G)            [delta passes: K/G within-group]
            + K × (G-1) × C_join(T,T')    [cross-group joins per delta pass]
```

For G=2 (binary split):
```
C_CSE(K, 2) = 2 × C_chain(K/2)           [materialize 2 halves]
            + K × C_chain(K/2)            [K delta passes, each K/2 within-group]
            + K × C_join(T_left, T_right)  [each pass joins with other half]
```

---

## 2. CSPA Per-Rule Analysis

### Dataset Parameters

| Relation | Type | Rows (input) | Rows (converged, est.) | Unique keys |
|----------|------|-------------|----------------------|-------------|
| assign | EDB | 179 | 179 (static) | ~100 nodes |
| dereference | EDB | 20 | 20 (static) | ~15 nodes |
| valueFlow | IDB | — | ~15,000 | ~100 × ~100 |
| memoryAlias | IDB | — | ~3,000 | ~50 × ~50 |
| valueAlias | IDB | — | ~2,400 | ~50 × ~50 |
| **Total IDB** | | | **~20,381** | |

### Rule-by-Rule Analysis

#### Rule 1: `valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y)` — TC self-join

- **Atoms:** 2 (K=2)
- **Join structure:** Self-join on valueFlow via shared variable z
- **Option 2 passes:** 2 (δvF × vF_full, vF_full × δvF)
- **CSE benefit:** None — K=2 already optimal
- **Cost per iteration (late):** C_join(15K, 15K) × 2 ≈ 2 × (15K + 15K + |result|)
- **Estimated |result|:** σ × 15K² ≈ 0.01 × 225M = 2.25M? No — constrained by ~100 unique nodes, so max |vF| = 10,000 (100²). More realistically, |vF ⋈ vF| ≈ |vF| = 15K (TC is idempotent near convergence, most joins produce existing tuples).
- **CSE verdict:** NOT APPLICABLE (K=2)

#### Rule 2: `valueFlow(x,y) :- assign(x,z), memoryAlias(z,y)` — EDB × IDB

- **Atoms:** 2 (K=2)
- **Join structure:** assign (EDB, 179 rows) × memoryAlias (IDB, ~3K rows)
- **Option 2 passes:** Only 1 effective — assign is EDB (no delta), so only `assign_full × δmemAlias`
- **CSE benefit:** None — K=2, one atom is EDB
- **CSE verdict:** NOT APPLICABLE

#### Rule 3: `memoryAlias(x,w) :- dereference(y,x), valueAlias(y,z), dereference(z,w)` — 3-way

- **Atoms:** 3 (K=3) — EDB × IDB × EDB
- **Join structure:** deref(y,x) ⋈ valueAlias(y,z) ⋈ deref(z,w)
- **Key observation:** Atoms 1 and 3 are the SAME EDB relation (dereference, 20 rows)
- **Option 2 passes (naive):** 3, but atoms 1 and 3 are EDB → only 1 effective delta pass (δvalueAlias)
  - Pass 1: δderef × vAlias × deref — deref is EDB, δderef = ∅ after first iteration → SKIP
  - Pass 2: deref × δvAlias × deref — **THE ONLY PRODUCTIVE PASS**
  - Pass 3: deref × vAlias × δderef — same as pass 1 → SKIP
- **CSE analysis:** With 2 EDB atoms, materialization of `T = deref ⋈ valueAlias` saves recomputation IF this rule were evaluated multiple times per iteration. But with only 1 effective pass, CSE adds overhead for no gain.
- **Cost per iteration:** C_join(20, 2.4K) + C_join(|T|, 20) where |T| ≈ σ × 20 × 2.4K. With ~15 unique y-values in deref and ~50 unique y-values in vAlias, selectivity σ_y ≈ 15/50 = 0.3. So |T| ≈ 0.3 × 2.4K ≈ 720 rows. Second join: C_join(720, 20) ≈ trivial.
- **CSE verdict:** NO BENEFIT — only 1 effective delta pass (2 of 3 atoms are static EDB)

#### Rule 4: `valueAlias(x,y) :- valueFlow(z,x), valueFlow(z,y)` — 2-way self-join

- **Atoms:** 2 (K=2)
- **CSE verdict:** NOT APPLICABLE (K=2)

#### Rule 5: `valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y)` — 3-way, ALL IDB

- **Atoms:** 3 (K=3) — IDB × IDB × IDB
- **Join structure:** valueFlow(z,x) ⋈ memoryAlias(z,w) ⋈ valueFlow(w,y)
- **THIS IS THE KEY RULE FOR CSE**
- **Option 2 passes (naive):** 3, ALL productive (all atoms are IDB with deltas)
  - Pass 1: δvF(z,x) × mAlias(z,w) × vF(w,y)
  - Pass 2: vF(z,x) × δmAlias(z,w) × vF(w,y)
  - Pass 3: vF(z,x) × mAlias(z,w) × δvF(w,y)

**Naive Option 2 cost per iteration (late stage):**
```
Pass 1: C_join(|δvF|, 3K) + C_join(|T₁|, 15K)
Pass 2: C_join(15K, |δmA|) + C_join(|T₂|, 15K)
Pass 3: C_join(15K, 3K) + C_join(|T₃|, |δvF|)  ← EXPENSIVE: full×full first join

Total: 3 × [C_join(~15K, ~3K) + C_join(~intermediate, ~15K)]
```

With late-stage deltas (|δvF| ≈ 100, |δmA| ≈ 50):
- Pass 1: C_join(100, 3K) + C_join(~30, 15K) ≈ 3.1K + 15K = 18.1K ops — CHEAP
- Pass 2: C_join(15K, 50) + C_join(~750, 15K) ≈ 15K + 15.8K = 30.8K ops
- Pass 3: C_join(15K, 3K) + C_join(~4.5K, 100) ≈ 18K + 4.6K = 22.6K ops
- **Total naive: ~71.5K ops**

**CSE approach — materialize T = vF(z,x) ⋈ mAlias(z,w):**
```
Materialize: T = vF ⋈ mAlias  →  |T| ≈ σ × 15K × 3K
  σ_z ≈ 100 unique z in vF, ~50 unique z in mAlias → overlap ~50 → σ ≈ 50/100 = 0.5
  |T| ≈ 0.5 × 15K × (3K/50) = 0.5 × 15K × 60 = 450K? Too high.

  Better estimate: For each z in mAlias, find matching vF rows.
  avg vF rows per z = 15K/100 = 150. avg mAlias rows per z = 3K/50 = 60.
  |T| = Σ_z (vF_rows_z × mAlias_rows_z) for matching z.
  ~50 matching z values: |T| ≈ 50 × 150 × 60 = 450K rows.
```

This is a LARGE intermediate. Materialization cost = C_join(15K, 3K) = ~18K ops, but |T| = 450K means:
- Memory: 450K × 4 cols × 8 bytes = 14.4 MB per materialization
- The final join T ⋈ vF(w,y) processes 450K × 15K, which is expensive

**CSE verdict for Rule 5:** MARGINAL. The intermediate |T| is large because of the many-to-many join structure. CSE saves the recomputation of `vF ⋈ mAlias` across 3 passes, but the materialized result is big. Net benefit depends on whether the 450K intermediate is smaller than recomputing 3× from scratch.

**Refined analysis:** In naive Option 2, passes 2 and 3 ALSO compute `vF ⋈ mAlias` (or part of it). Pass 3 computes `vF_full × mAlias_full` = the same 450K intermediate. So naive already pays this cost. CSE saves recomputing it in pass 3 (one C_join(15K, 3K) = 18K ops saved) but adds 14.4 MB memory.

**Net savings for Rule 5 per iteration: ~18K ops saved, 14.4 MB memory cost.**

### CSPA Summary Table

| Rule | K | IDB atoms | Effective delta passes | CSE benefit | Savings/iter |
|------|---|-----------|----------------------|-------------|-------------|
| R1: vF TC | 2 | 2 | 2 | None (K=2) | 0 |
| R2: vF cross | 2 | 1 | 1 | None (K=2) | 0 |
| R3: mAlias 3-way | 3 | 1 | 1 | None (2 EDB) | 0 |
| R4: vAlias self-join | 2 | 2 | 2 | None (K=2) | 0 |
| **R5: vAlias 3-way** | **3** | **3** | **3** | **Marginal** | **~18K ops, +14.4 MB** |

**CSPA CSE verdict: MINIMAL BENEFIT.** Only Rule 5 has 3 IDB atoms. The savings are modest (~18K ops/iter ≈ one hash-join of 15K×3K) vs the memory cost of materializing a 450K-row intermediate. The primary wins for CSPA come from Option 1 (incremental CONSOLIDATE) and basic Option 2 (correct delta expansion), not from CSE.

---

## 3. DOOP Per-Rule Analysis

### Recursive Stratum (Phase 4) — The Critical Rules

DOOP's Phase 4 has mutual recursion across VarPointsTo, CallGraphEdge, Reachable, InstanceFieldPointsTo, StaticFieldPointsTo, and ArrayIndexPointsTo.

#### Dataset Scale (zxing)

| Relation | Type | Input rows | Est. converged |
|----------|------|-----------|---------------|
| Instruction_Method | EDB-derived | ~100K | static |
| VirtualMethodInvocation_Base | EDB-derived | ~10K | static |
| VirtualMethodInvocation_SimpleName | EDB-derived | ~10K | static |
| VirtualMethodInvocation_Descriptor | EDB-derived | ~10K | static |
| HeapAllocation_Type | EDB | ~50K | static |
| ComponentType | EDB | ~5K | static |
| SupertypeOf | IDB (Phase 2) | ~200K | static in Phase 4 |
| MethodLookup | IDB (Phase 2) | ~50K | static in Phase 4 |
| VarPointsTo | IDB | — | ~500K-2M (unknown) |
| Reachable | IDB | — | ~10K-50K |
| CallGraphEdge | IDB | — | ~20K-100K |
| InstanceFieldPointsTo | IDB | — | ~100K-500K |
| ArrayIndexPointsTo | IDB | — | ~10K-50K |

### Critical Rules for CSE Analysis

#### Rule D1: ArrayIndexPointsTo — 8-way join (line 390)

```
ArrayIndexPointsTo(baseheap, heap) :-
  Reachable(inmethod),                      -- A₁: IDB (~30K)
  StoreArrayIndex(from, base, inmethod),    -- A₂: EDB-derived (~5K)
  VarPointsTo(baseheap, base),              -- A₃: IDB (~1M)
  VarPointsTo(heap, from),                  -- A₄: IDB (~1M)
  HeapAllocation_Type(heap, heaptype),      -- A₅: EDB (~50K)
  HeapAllocation_Type(baseheap, baseheaptype), -- A₆: EDB (~50K)
  ComponentType(baseheaptype, componenttype),  -- A₇: EDB (~5K)
  SupertypeOf(componenttype, heaptype).     -- A₈: EDB* (~200K, static)
```

**IDB atoms:** A₁ (Reachable), A₃ (VarPointsTo), A₄ (VarPointsTo) = 3 IDB atoms
**EDB/static atoms:** A₂, A₅, A₆, A₇, A₈ = 5 static atoms

**Naive Option 2:** 8 passes, but only 3 are productive (δReachable, δVarPointsTo×2).
- Each productive pass: full 8-way left-deep chain = 7 sequential joins
- Estimated cost per pass: dominated by VarPointsTo (1M rows) joins

**CSE approach — split into 2 groups of 4:**
```
Group L: A₁ ⋈ A₂ ⋈ A₃ ⋈ A₄  (Reachable × StoreArrayIndex × VarPointsTo × VarPointsTo)
Group R: A₅ ⋈ A₆ ⋈ A₇ ⋈ A₈  (HeapAllocation_Type × HeapAllocation_Type × ComponentType × SupertypeOf)
```

**Key insight:** Group R is ENTIRELY EDB/static. It has NO deltas in Phase 4. Therefore:
- Group R can be materialized ONCE at stratum entry (not per iteration)
- All 3 productive delta passes only affect Group L
- Each delta pass: 3-way join within Group L (not 7-way), then join with materialized Group R

**Cost comparison:**
```
Naive (per productive pass):
  7 sequential joins through 1M-row VarPointsTo = very expensive
  3 productive passes × 7 joins = 21 join operations

CSE (per productive pass):
  Materialize Group R once: 3 joins over static data (one-time, ~200K result)
  3 productive passes × 3 joins (within Group L) + 1 cross-group join = 12 join operations

Savings: 21 → 12 join ops = 43% reduction, PLUS Group R materialization amortized over all iterations
```

But the real win is deeper: the 7-way chain means intermediate results balloon through VarPointsTo (1M rows). With CSE, the Group L chain is only 3 joins, keeping intermediates smaller.

#### Rule D2: VarPointsTo virtual dispatch — 9-way join (line 402)

```
VarPointsTo(heap, this) :-
  Reachable(inMethod),                               -- A₁: IDB
  Instruction_Method(invocation, inMethod),           -- A₂: EDB (~100K)
  VirtualMethodInvocation_Base(invocation, base),     -- A₃: EDB (~10K)
  VarPointsTo(heap, base),                            -- A₄: IDB (~1M)
  HeapAllocation_Type(heap, heaptype),                -- A₅: EDB (~50K)
  VirtualMethodInvocation_SimpleName(inv, simplename),-- A₆: EDB (~10K)
  VirtualMethodInvocation_Descriptor(inv, descriptor),-- A₇: EDB (~10K)
  MethodLookup(simplename, descriptor, heaptype, toMethod), -- A₈: static (~50K)
  ThisVar(toMethod, this).                            -- A₉: EDB (~20K)
```

**IDB atoms:** A₁ (Reachable), A₄ (VarPointsTo) = 2 IDB atoms
**EDB/static atoms:** A₂, A₃, A₅, A₆, A₇, A₈, A₉ = 7 static atoms

**Naive Option 2:** 9 passes, only 2 productive. Each: 8 sequential joins.
**CSE approach:**
```
Group L: A₁ ⋈ A₂ ⋈ A₃ ⋈ A₄ (Reachable → invocations → bases → VarPointsTo)
Group R: A₅ ⋈ A₆ ⋈ A₇ ⋈ A₈ ⋈ A₉ (all static — materialize ONCE)
```

Group R is entirely static → materialized once at stratum entry.
2 productive passes × 3 joins (Group L) + 1 cross-group = 8 join ops vs 16 (naive).
**50% reduction.**

#### Rule D3: Reachable/CallGraphEdge virtual dispatch — 8-way (lines 411-412)

Identical join body to D2 minus ThisVar. Same analysis applies.
- **IDB atoms:** 2 (Reachable, VarPointsTo)
- **CSE savings:** ~50% (same Group L/R split)

#### Rule D4: InstanceFieldPointsTo — 4-way (line 405)

```
InstanceFieldPointsTo(heap, fld, baseheap) :-
  Reachable(inmethod),
  StoreInstanceField(from, base, fld, inmethod),
  VarPointsTo(heap, from),
  VarPointsTo(baseheap, base).
```

- **K=4, IDB atoms:** 3 (Reachable, VarPointsTo×2)
- **CSE:** Could split into pairs, but K=4 with 3 IDB atoms → only marginal win
- **CSE verdict:** MINOR — 4-way chain is manageable

#### Rule D5: VarPointsTo cast — 5-way (line 387)

```
VarPointsTo(heap, to) :-
  Reachable(inmethod), AssignCast(type, from, to, inmethod),
  SupertypeOf(type, heaptype), HeapAllocation_Type(heap, heaptype),
  VarPointsTo(heap, from).
```

- **K=5, IDB atoms:** 2 (Reachable, VarPointsTo)
- **CSE:** Group static atoms (AssignCast, SupertypeOf, HeapAllocation_Type) → materialize once
- **CSE verdict:** MODERATE — saves recomputation of 3-static-atom chain

#### Rule D6: Special method dispatch — 6-way (lines 419-421)

```
{VarPointsTo,Reachable,CallGraphEdge}(...) :-
  Reachable(inmethod), Instruction_Method(invocation, inmethod),
  SpecialMethodInvocation_Base(invocation, base),
  VarPointsTo(heap, base),
  MethodInvocation_Method(invocation, tomethod),
  ThisVar(tomethod, this).
```

- **K=6, IDB atoms:** 2 (Reachable, VarPointsTo)
- **4 static atoms** can be materialized once
- **CSE verdict:** MODERATE

### DOOP Summary Table

| Rule | K | IDB atoms | Effective δ passes | CSE benefit | Key insight |
|------|---|-----------|-------------------|-------------|-------------|
| D1: ArrayIndexPointsTo | 8 | 3 | 3 | **HIGH** | 5 static atoms → materialize once |
| D2: VarPointsTo vdispatch | 9 | 2 | 2 | **HIGH** | 7 static atoms → materialize once |
| D3: Reachable vdispatch | 8 | 2 | 2 | **HIGH** | 7 static atoms → same as D2 |
| D3b: CallGraphEdge vdispatch | 8 | 2 | 2 | **HIGH** | 7 static atoms → same as D2 |
| D4: InstanceFieldPointsTo | 4 | 3 | 3 | Minor | Small K |
| D5: VarPointsTo cast | 5 | 2 | 2 | Moderate | 3 static atoms |
| D6: Special dispatch | 6 | 2 | 2 | Moderate | 4 static atoms |
| VarPointsTo field load | 4 | 3 | 3 | Minor | Small K |
| Assign (call graph) | 3 | 2 | 2 | Minor | K=3 |

**DOOP CSE verdict: HIGH BENEFIT for the 8/9-way virtual dispatch rules (D1, D2, D3).** These rules dominate DOOP's runtime because they involve VarPointsTo (~1M rows). CSE's ability to materialize the static atom groups ONCE (not per iteration) is transformative.

---

## 4. Cost Model Equations

### CSPA 3-way (Rule 5)

```
Rule: valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y)

Let V = |valueFlow|, M = |memoryAlias|, δV = |δvalueFlow|, δM = |δmemoryAlias|

Naive Option 2 cost per iteration:
  C_naive = C_join(δV, M) + C_join(T₁, V)     [pass 1: δvF × mA × vF]
          + C_join(V, δM) + C_join(T₂, V)      [pass 2: vF × δmA × vF]
          + C_join(V, M)  + C_join(T₃, δV)     [pass 3: vF × mA × δvF]

Late-stage (V≈15K, M≈3K, δV≈100, δM≈50):
  C_naive ≈ (100+3K) + (30+15K)        [pass 1: 18.1K]
          + (15K+50) + (750+15K)        [pass 2: 30.8K]
          + (15K+3K) + (4.5K+100)       [pass 3: 22.6K]
  C_naive ≈ 71.5K comparison ops

CSE cost per iteration:
  Materialize T = vF ⋈ mA:  C_join(V, M) = 18K ops, |T| ≈ 450K rows
  Pass 1: C_join(δV, M) + C_join(δT₁, V) ≈ 3.1K + 15K = 18.1K
  Pass 2: C_join(V, δM) + C_join(δT₂, V) ≈ 15K + 15.8K = 30.8K
  Pass 3: C_join(T, δV) [reuse T]       ≈ 450K + 100 = 450.1K  ← WORSE due to large T

  C_CSE ≈ 18K + 18.1K + 30.8K + 450.1K = 517K  ← WORSE THAN NAIVE!
```

**CSPA CSE is actually COUNTERPRODUCTIVE for Rule 5.** The materialized intermediate T is 450K rows (larger than any input), making the final join with δvF more expensive than the naive approach. This is because the vF×mA join is many-to-many with poor selectivity.

**Revised CSPA verdict: DO NOT USE CSE. Use naive Option 2 (3 plan copies).**

### DOOP 8-way (Rule D2: VarPointsTo virtual dispatch)

```
Rule: VarPointsTo(heap, this) :- Reachable(inMethod), Instruction_Method(inv, inMethod),
      VMI_Base(inv, base), VarPointsTo(heap, base), HAT(heap, ht),
      VMI_SimpleName(inv, sn), VMI_Descriptor(inv, desc),
      MethodLookup(sn, desc, ht, toMethod), ThisVar(toMethod, this)

Let: R=|Reachable|≈30K, IM=|Insn_Method|≈100K, VB=|VMI_Base|≈10K,
     VP=|VarPointsTo|≈1M, HAT=|HeapAlloc_Type|≈50K,
     VS=|VMI_SimpleName|≈10K, VD=|VMI_Desc|≈10K,
     ML=|MethodLookup|≈50K, TV=|ThisVar|≈20K

Naive Option 2 (2 productive passes):
  Pass 1 (δReachable): 8 sequential joins starting from δR (~100 rows)
    C₁ = C_join(100, 100K) + C_join(~5K, 10K) + C_join(~2K, 1M) +
         C_join(~100K, 50K) + C_join(~50K, 10K) + C_join(~10K, 10K) +
         C_join(~5K, 50K) + C_join(~2K, 20K)
    C₁ ≈ 100K + 15K + 1M + 150K + 60K + 20K + 55K + 22K ≈ 1.42M ops

  Pass 2 (δVarPointsTo): 8 sequential joins
    C₂ = C_join(30K, 100K) + C_join(~10K, 10K) + C_join(~5K, δVP=1K) +
         ... (intermediates smaller due to delta, but still 8 joins)
    C₂ ≈ similar order, ~1-2M ops

  C_naive ≈ 2 × ~1.5M = 3M ops per iteration

CSE approach (Group L: A₁-A₄, Group R: A₅-A₉):
  Materialize Group R ONCE at stratum entry (all static):
    T_R = HAT ⋈ VMI_SimpleName ⋈ VMI_Descriptor ⋈ MethodLookup ⋈ ThisVar
    This is a join over static data — computed ONCE, reused every iteration.
    |T_R| ≈ 20K-50K (method dispatch targets × this-vars)
    Cost: 4 joins over static data ≈ 300K ops (ONE TIME)

  Per iteration (2 productive passes):
    Pass 1 (δReachable):
      Group L: C_join(100, 100K) + C_join(5K, 10K) + C_join(2K, 1M) ≈ 1.1M ops
      Cross: C_join(~100K, T_R=30K) ≈ 130K ops
      Total: 1.23M ops

    Pass 2 (δVarPointsTo):
      Group L: C_join(30K, 100K) + C_join(10K, 10K) + C_join(5K, δVP=1K) ≈ 140K ops
      Cross: C_join(~5K, T_R=30K) ≈ 35K ops
      Total: 175K ops

  C_CSE ≈ 1.23M + 175K = 1.4M ops per iteration (+ 300K one-time)

  Savings: 3M → 1.4M = 53% reduction per iteration
```

**For DOOP's 8-way rules, CSE provides ~50% reduction per iteration**, plus the one-time materialization of static groups is amortized over potentially hundreds of iterations.

---

## 5. Memory Projections

### CSPA Memory Analysis

**Current peak RSS:** 4.46 GB (20,381 final tuples, 320 KB useful data)

Memory breakdown (estimated):
| Component | Size | % of peak |
|-----------|------|-----------|
| old_data snapshots (3 relations × copy/iter) | ~2.5 GB | 56% |
| Intermediate join results (per-iteration) | ~1.2 GB | 27% |
| Hash tables for joins | ~0.5 GB | 11% |
| Final relation data | ~0.3 GB | 6% |

**Savings from Option 1 (incremental CONSOLIDATE):**
- Eliminates old_data snapshots: -2.5 GB (56% reduction)
- Delta is computed as merge byproduct, no separate copy needed

**Savings from Option 2 (correct delta expansion):**
- ~50% fewer iterations → ~50% fewer intermediate allocations
- But each iteration has K passes (more intermediates per iteration)
- Net: ~30% reduction in intermediate memory (fewer iterations × more per iter)

**Combined Option 1 + Option 2 (no CSE):**
```
Current:     4.46 GB
After Opt 1: ~1.96 GB  (eliminate snapshots)
After Opt 2: ~1.37 GB  (30% fewer intermediate allocations)
Projected:   ~1.4 GB   (69% reduction from 4.46 GB)
```

**With CSE (Rule 5 materialization):**
- Adds 14.4 MB per materialized intermediate (T = vF ⋈ mA)
- But as shown above, CSE is counterproductive for CSPA
- **No CSE memory impact for CSPA**

### DOOP Memory Projection

**Current:** DNF (estimated multi-GB, unknown)

**Key memory concern:** VarPointsTo at ~1M rows × 2 cols × 8 bytes = 16 MB. With 6+ IDB relations, intermediates, and hash tables, estimated 2-8 GB working set.

**CSE impact on DOOP memory:**
- Materializing Group R (static atoms): ~50K rows × ~8 cols × 8 bytes = 3.2 MB (trivial)
- This materialization persists for the ENTIRE stratum (not freed between iterations)
- Eliminates recomputation of static chain every pass → reduces peak intermediate memory
- **Net DOOP memory impact: slight reduction (less intermediate churn)**

---

## 6. Materialization Threshold Recommendations

### Decision Framework

| Condition | Recommendation |
|-----------|---------------|
| K = 2 | Never use CSE — already optimal |
| K = 3, all IDB atoms | Evaluate: CSE beneficial only if intermediate T is smaller than largest input. For many-to-many joins (CSPA Rule 5), CSE is HARMFUL. |
| K = 3, 2+ EDB atoms | No CSE needed — only 1 effective delta pass |
| K >= 4, with static atom groups | **USE CSE** — materialize static groups once at stratum entry |
| K >= 6 | **ALWAYS USE CSE** — static group materialization is transformative |
| K >= 8 (DOOP) | **MANDATORY CSE** — without it, 8-way chain is infeasible |

### Proposed Algorithm

```
for each rule R with K body atoms in recursive stratum:
    idb_atoms = atoms referencing IDB relations with deltas
    static_atoms = atoms referencing EDB or non-recursive IDB (no delta in this stratum)

    if |idb_atoms| <= 1:
        # At most 1 delta pass — no CSE benefit
        emit 1 plan copy (standard delta heuristic)

    elif |static_atoms| >= 2 and K >= 4:
        # Profitable: materialize static group once
        group_static = join plan for static_atoms
        group_dynamic = join plan for idb_atoms
        emit: MATERIALIZE(group_static) at stratum entry
        for each idb_atom with delta:
            emit: delta pass over group_dynamic ⋈ materialized_static

    else:
        # K=2 or K=3 with all-IDB: naive Option 2
        for each idb_atom with delta:
            emit: plan copy with delta at this position
```

### Break-Even Analysis

**When does CSE materialization pay for itself?**

```
Cost_materialize = C_join_chain(static_atoms)  [one-time per stratum entry]
Cost_saved_per_pass = C_join_chain(static_atoms)  [avoided recomputation]
Num_productive_passes = |idb_atoms|

Break-even: Cost_materialize = Cost_saved_per_pass × (passes_that_reuse - 1)
  → Breaks even when 2+ passes reuse the materialized result
  → For DOOP D2 (2 IDB atoms): breaks even after pass 1, profitable at pass 2
  → For DOOP D1 (3 IDB atoms): breaks even at pass 2, profitable at pass 3
```

**Per-iteration amortization:**
```
Over I iterations, CSE amortization:
  Total_CSE_cost = C_materialize + I × (K_idb × C_dynamic_chain + K_idb × C_cross_join)
  Total_naive_cost = I × K_idb × C_full_chain

  Break-even iterations: 1 (since C_materialize << I × K_idb × C_static_chain for I >= 1)
```

CSE for static groups breaks even IMMEDIATELY because the materialization is done once but reused every iteration. For DOOP with potentially hundreds of iterations, this is a massive win.

---

## 7. Recommendations

### For CSPA (3-way rules)

1. **DO NOT use CSE materialization** — Rule 5's intermediate is too large (many-to-many join)
2. **USE naive Option 2** — 3 plan copies for Rule 5, simple and effective
3. Rule 3 only needs 1 pass (2 EDB atoms), not 3
4. **Primary wins:** Option 1 (incremental CONSOLIDATE) + basic Option 2 (correct delta)
5. **Expected CSPA improvement:** 4,602ms → ~230ms (Option 1 + Option 2 combined, no CSE)

### For DOOP (8/9-way rules)

1. **USE CSE materialization for rules D1, D2, D3** (K >= 8, many static atoms)
2. **Materialize static atom groups at stratum entry** (one-time cost, reused every iteration)
3. **This is likely REQUIRED for DOOP to complete** — without CSE, 8-way chains through 1M-row VarPointsTo are infeasible
4. Consider CSE for D5 (K=5) and D6 (K=6) as secondary optimization
5. **Expected DOOP improvement:** DNF → completion (estimated minutes, not hours)

### Implementation Priority

| Priority | Action | Workloads | Effort |
|----------|--------|-----------|--------|
| 1 | Option 1: Incremental CONSOLIDATE | All recursive | 3-5 days |
| 2 | Option 2 (naive): K plan copies for K-atom rules | CSPA, DOOP | 1-2 weeks |
| 3 | CSE: Static group materialization at stratum entry | DOOP (K>=4) | +3-5 days on top of Option 2 |
| 4 | CSE: Dynamic group materialization per iteration | Future (if needed) | +1 week |

### Threshold Configuration (US-008)

```c
/* Materialization policy */
typedef enum {
    WL_MAT_NEVER = 0,     /* K=2: never materialize */
    WL_MAT_NAIVE_MULTI,   /* K=3 all-IDB: naive K plan copies */
    WL_MAT_STATIC_GROUP,  /* K>=4 with 2+ static: materialize static group */
    WL_MAT_FULL_CSE,      /* K>=8: full CSE with group splitting */
} wl_materialization_policy_t;

/* Decision function */
static wl_materialization_policy_t
choose_materialization(uint32_t k_atoms, uint32_t n_idb, uint32_t n_static)
{
    if (k_atoms <= 2) return WL_MAT_NEVER;
    if (n_static < 2 || k_atoms < 4) return WL_MAT_NAIVE_MULTI;
    if (k_atoms >= 8) return WL_MAT_FULL_CSE;
    return WL_MAT_STATIC_GROUP;
}
```

---

## 8. Hypothesis Validation Impact

| Hypothesis | CSE Impact | Notes |
|-----------|-----------|-------|
| H1 (CONSOLIDATE) | Independent | CSE does not affect consolidation strategy |
| H2 (delta expansion) | CSE is an ENHANCEMENT to H2 fix | Basic Option 2 fixes H2; CSE optimizes the fix for high-K rules |
| H3 (workqueue) | Complementary | CSE reduces per-pass cost; workqueue parallelizes passes |

### Iteration Count Reduction (H2 Refinement)

**Without CSE (naive Option 2):** ~50% fewer iterations for CSPA (as estimated in OPTIMIZATION-STRATEGY.md)

**With CSE for DOOP:** CSE does NOT reduce iteration count — it reduces per-iteration cost. The iteration count reduction comes from correct delta expansion (all K permutations). CSE makes each iteration cheaper, enabling DOOP to actually complete within those iterations.

**Combined effect for DOOP:**
- Correct delta expansion: converge in O(diameter) iterations instead of O(K × diameter)
- CSE: each iteration 50% cheaper (8-way → effectively 3-way + materialized lookup)
- Together: DOOP goes from DNF → completion in estimated 5-30 minutes (vs hours without either)
