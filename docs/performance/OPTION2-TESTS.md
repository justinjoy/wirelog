# Option 2 + CSE Materialization: Test Specification

**Date:** 2026-03-07
**Status:** Design Phase (US-001)
**Companion:** `docs/performance/OPTION2-DESIGN.md`

---

## Overview

This document specifies 25 test cases for the CSE + Progressive Materialization approach
to multi-way delta expansion. Tests are organized in five categories: unit, integration,
regression, edge case, and performance.

All test cases follow the existing wirelog test conventions: custom TEST/PASS/FAIL macros,
each test file has its own `main()`, registered in `tests/meson.build`.

---

## 1. Unit Tests: Core Data Structures

### T-U01: Materialization Cache Init/Free

**Purpose:** Verify `col_mat_cache_t` lifecycle.

```
GIVEN  a freshly initialized col_mat_cache_t with 64MB limit
WHEN   count is checked
THEN   count == 0, mem_used == 0, mem_limit == 64MB
WHEN   cache is freed
THEN   no memory leaks (valgrind clean)
```

### T-U02: Cache Store and Lookup (Happy Path)

**Purpose:** Verify basic store/retrieve cycle for materialized joins.

```
GIVEN  a cache with 64MB limit
AND    a relation R with 100 rows, 3 cols
WHEN   col_op_materialize(cache, rule_idx=0, prefix_depth=1, R, atom_nrows=[100,50], atom_count=3)
THEN   cache.count == 1, cache.mem_used == 100*3*8 bytes
WHEN   col_op_lookup_materialized(cache, rule_idx=0, prefix_depth=1, atom_nrows=[100,50])
THEN   returns R (non-NULL, same data)
```

### T-U03: Cache Staleness Detection

**Purpose:** Verify stale entries are not returned.

```
GIVEN  a cached entry with atom_versions=[100, 50] at rule 0, depth 1
WHEN   lookup with atom_nrows=[100, 51]  (atom 1 changed)
THEN   returns NULL (stale)
WHEN   lookup with atom_nrows=[101, 50]  (atom 0 changed)
THEN   returns NULL (stale)
WHEN   lookup with atom_nrows=[100, 50]  (unchanged)
THEN   returns cached result (not stale)
```

### T-U04: Cache Memory Limit and Eviction

**Purpose:** Verify LRU eviction when memory limit is exceeded.

```
GIVEN  a cache with mem_limit = 1000 bytes
AND    entry A stored (500 bytes, rule 0, depth 1)
AND    entry B stored (500 bytes, rule 1, depth 1)
WHEN   entry C is stored (600 bytes, rule 2, depth 1)
THEN   entry A is evicted (LRU: oldest first)
AND    cache.count == 2 (B and C)
AND    cache.mem_used == 1100 bytes  -- wait, should be <= 1000
THEN   actually, B is also evicted if needed until C fits
AND    cache.mem_used <= 1000
```

### T-U05: Cache Eviction Fails Gracefully

**Purpose:** Verify behavior when a single entry exceeds memory limit.

```
GIVEN  a cache with mem_limit = 100 bytes
WHEN   col_op_materialize with a 200-byte result
THEN   returns ENOMEM
AND    cache.count == 0 (nothing stored)
AND    caller falls back to recomputation
```

### T-U06: Empty Delta Produces Empty Result

**Purpose:** Verify `col_op_variable` with `WL_DELTA_FORCE_DELTA` and empty delta.

```
GIVEN  a session with relation "R" (50 rows) but no "$d$R" delta relation
WHEN   col_op_variable executes with delta_mode = WL_DELTA_FORCE_DELTA
THEN   pushes an empty relation onto the eval stack
AND    is_delta flag is true
```

*Reference:* `columnar_nanoarrow.c:602-611`

### T-U07: Force Full Ignores Delta

**Purpose:** Verify `col_op_variable` with `WL_DELTA_FORCE_FULL`.

```
GIVEN  a session with "R" (50 rows) and "$d$R" (5 rows)
WHEN   col_op_variable executes with delta_mode = WL_DELTA_FORCE_FULL
THEN   pushes the full relation (50 rows) onto the eval stack
AND    is_delta flag is false
```

*Reference:* `columnar_nanoarrow.c:599-601`

### T-U08: JOIN Force Delta for Right Side

**Purpose:** Verify `col_op_join` with `WL_DELTA_FORCE_DELTA`.

```
GIVEN  left relation L (10 rows), right relation "R" (50 rows), "$d$R" (3 rows)
WHEN   col_op_join executes with delta_mode = WL_DELTA_FORCE_DELTA
THEN   joins L with "$d$R" (3 rows), NOT full R (50 rows)
AND    result row count <= 10 * 3 (upper bound for matching keys)
```

*Reference:* `columnar_nanoarrow.c:782-800`

### T-U09: JOIN Force Delta No Delta Available

**Purpose:** Verify `col_op_join` produces empty result when right delta is missing.

```
GIVEN  left relation L (10 rows), right relation "R" (50 rows), no "$d$R"
WHEN   col_op_join executes with delta_mode = WL_DELTA_FORCE_DELTA
THEN   pushes an empty relation onto eval stack
AND    is_delta flag is true
```

*Reference:* `columnar_nanoarrow.c:789-800`

---

## 2. Unit Tests: Multi-Pass Delta Expansion Logic

### T-U10: Atom Count Detection from Plan Ops

**Purpose:** Verify correct identification of K atoms from a plan's op sequence.

```
GIVEN  a relation plan with ops: [VARIABLE, JOIN, JOIN, MAP, CONSOLIDATE]
WHEN   atom count is computed (count VARIABLEs at op[0] + JOINs)
THEN   atom_count == 3 (1 VARIABLE + 2 JOINs)
AND    var_indices = [0]
AND    join_indices = [1, 2]
```

### T-U11: 3-Atom Rule Produces 3 Delta Variants

**Purpose:** Verify multi-pass loop generates all 3 permutations for a 3-way join.

```
GIVEN  rule R(x,w) :- A(y,x), B(y,z), C(z,w)
AND    A_full = {(1,10),(2,20)}, B_full = {(1,100),(2,200)}, C_full = {(100,1000)}
AND    delta(A) = {(3,30)}, delta(B) = {(1,101)}, delta(C) = {(200,2000)}
WHEN   col_op_union_delta_variants is called
THEN   result includes tuples from all 3 passes:
       Pass 0: delta(A) x B_full x C_full
       Pass 1: A_full x delta(B) x C_full
       Pass 2: A_full x B_full x delta(C)
AND    result is the union of all 3 passes
```

### T-U12: Skip Pass When Delta Is Empty

**Purpose:** Verify passes with empty deltas are skipped.

```
GIVEN  rule R :- A, B, C  where A is EDB (no delta)
AND    delta(B) has rows, delta(C) has rows
WHEN   col_op_union_delta_variants is called
THEN   only passes 1 and 2 execute (pass 0 skipped: delta(A) is empty)
AND    result contains tuples from passes 1 and 2 only
```

---

## 3. Integration Tests: Real Workload Rules

### T-I01: CSPA 3-Way Join Correctness (valueAlias)

**Purpose:** End-to-end correctness for CSPA's 3-way join rule with CSE.

```
GIVEN  the CSPA workload (bench/workloads/cspa.dl)
AND    test dataset (bench/data/graph_10.csv derivatives)
WHEN   evaluated with CSE multi-pass delta expansion enabled
THEN   final tuple counts for all CSPA relations match baseline:
       valueFlow, memoryAlias, valueAlias counts identical to non-CSE run
```

### T-I02: CSPA 3-Way Join Correctness (memoryAlias)

**Purpose:** Verify memoryAlias rule (EDB + IDB + EDB atoms).

```
GIVEN  the CSPA workload with:
       dereference = {(1,10),(2,20),(3,30)}
       assign = {(10,20),(20,30)}
WHEN   evaluated to fixed point
THEN   memoryAlias contains expected reflexive + transitive closure tuples
AND    results match single-pass evaluation (baseline oracle)
```

### T-I03: DOOP 8-Way Join Correctness (CallGraphEdge)

**Purpose:** End-to-end correctness for DOOP's 8-way virtual dispatch rule.

```
GIVEN  the DOOP workload (bench/workloads/doop.dl)
AND    zxing dataset
WHEN   evaluated with CSE multi-pass delta expansion enabled
THEN   final tuple counts for CallGraphEdge, Reachable, VarPointsTo
       match baseline (non-CSE evaluation)
```

### T-I04: DOOP 9-Way Join Correctness (VarPointsTo via virtual dispatch)

**Purpose:** Verify the 9-way join rule in DOOP.

```
GIVEN  the DOOP workload
WHEN   VarPointsTo rule at doop.dl:402 is evaluated with CSE
THEN   VarPointsTo tuple count matches baseline
AND    all downstream rules (InstanceFieldPointsTo, etc.) also match
```

### T-I05: Mixed K Rules in Same Stratum

**Purpose:** Verify correctness when a stratum has both 2-atom and 3+ atom rules.

```
GIVEN  CSPA stratum with:
       valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y).  -- 2-atom (no CSE)
       valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y). -- 3-atom (CSE)
WHEN   evaluated together in same stratum
THEN   2-atom rules use original path (WL_DELTA_AUTO)
AND    3-atom rules use CSE multi-pass path
AND    all tuple counts match baseline
```

---

## 4. Regression Tests: Non-Option2 Rules Unaffected

### T-R01: 2-Atom Recursive Rules Unchanged

**Purpose:** Verify the CSE path does not activate for 2-atom rules.

```
GIVEN  transitive closure: tc(x,y) :- edge(x,z), tc(z,y).
WHEN   evaluated with CSE code present
THEN   uses original evaluation path (atom_count < 3)
AND    tuple count matches baseline exactly
AND    iteration count matches baseline exactly
```

### T-R02: Non-Recursive Strata Unchanged

**Purpose:** Verify CSE does not affect non-recursive evaluation.

```
GIVEN  non-recursive rules (e.g., DOOP Phase 1 decomposition)
WHEN   evaluated with CSE code present
THEN   non-recursive path (columnar_nanoarrow.c:1550-1617) is taken
AND    no materialization cache is created
AND    tuple counts match baseline
```

### T-R03: All 15 Benchmark Workloads Pass

**Purpose:** Full regression suite.

```
FOR EACH workload in [TC, Reach, CC, SSSP, SG, Bipartite, Andersen,
                      CSPA, CSDA, Dyck-2, Galen, Polonius, DDISASM, CRDT, DOOP]:
WHEN   evaluated with CSE enabled
THEN   final tuple counts for ALL output relations match baseline
```

### T-R04: Negation Rules Unaffected

**Purpose:** Verify ANTIJOIN rules are not broken by CSE.

```
GIVEN  DOOP MethodLookup rule with negation:
       MethodLookup(sn,d,t,m) :- DirectSuperclass(t,st), MethodLookup(sn,d,st,m),
                                  !MethodImplemented(sn,d,t,_).
WHEN   evaluated with CSE code present
THEN   negated atom is never assigned delta position
AND    MethodLookup tuple count matches baseline
```

---

## 5. Edge Case Tests

### T-E01: Self-Join Rule (Same Relation Twice)

**Purpose:** Verify correct delta handling when the same relation appears multiple times.

```
GIVEN  rule: valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y).
       (This is a 2-atom self-join, handled by existing path, but verify)
WHEN   evaluated
THEN   pass 0: delta(vF) x vF_full
AND    pass 1: vF_full x delta(vF)
AND    both passes produce correct results
AND    no double-counting after consolidation
```

### T-E02: Self-Join in 3-Way Rule

**Purpose:** Verify CSE when the same relation appears in multiple positions.

```
GIVEN  rule: valueAlias(x,y) :- valueFlow(z,x), memoryAlias(z,w), valueFlow(w,y).
       (valueFlow appears at positions 0 and 2)
WHEN   delta(valueFlow) is non-empty
THEN   pass 0 uses delta(vF) at position 0, full(vF) at position 2
AND    pass 2 uses full(vF) at position 0, delta(vF) at position 2
AND    pass 0 and pass 2 may produce overlapping tuples (deduped by CONSOLIDATE)
```

### T-E03: Cycle Detection (Mutual Recursion)

**Purpose:** Verify CSE works correctly with mutually recursive relations.

```
GIVEN  CSPA: valueFlow, memoryAlias, valueAlias are mutually recursive
WHEN   iteration N produces delta(valueFlow) and delta(memoryAlias)
THEN   valueAlias evaluation sees both deltas
AND    all 3 passes for valueAlias use correct delta/full combinations
AND    fixed-point is reached with correct tuple counts
```

### T-E04: All Deltas Empty (Fixed Point)

**Purpose:** Verify termination when no relation has new facts.

```
GIVEN  a stratum at fixed point (all deltas empty)
WHEN   col_op_union_delta_variants is called
THEN   all K passes are skipped (every delta is empty)
AND    result is empty
AND    the outer loop detects any_new == false and terminates
```

### T-E05: Single-Row Delta

**Purpose:** Verify correctness with minimal delta.

```
GIVEN  rule R :- A, B, C  with delta(B) = {(1, 2)} (single row)
WHEN   pass 1 is evaluated
THEN   result = A_full x {(1,2)} x C_full
AND    result is correct (single-row delta still joins correctly)
```

---

## 6. Performance Tests

### T-P01: Iteration Count Reduction (CSPA)

**Purpose:** Measure whether CSE reduces CSPA iteration count.

```
GIVEN  CSPA workload with standard dataset
WHEN   evaluated with CSE vs without CSE
THEN   iteration count with CSE <= iteration count without CSE
AND    expected reduction: 10-30% (hypothesis H2 from OPTIMIZATION-STRATEGY.md)
RECORD iteration_count_baseline, iteration_count_cse, reduction_pct
```

### T-P02: Wall Time Not Regressed (CSPA)

**Purpose:** Ensure CSE does not cause regression like original Option 2.

```
GIVEN  CSPA workload
WHEN   evaluated with CSE enabled
THEN   wall_time_cse <= wall_time_baseline * 1.1  (at most 10% slower)
RECORD wall_time_baseline, wall_time_cse
NOTE   Original Option 2 caused 8.4x regression; this is the critical gate.
```

### T-P03: Memory Overhead Bounded

**Purpose:** Verify materialization cache memory stays within limits.

```
GIVEN  CSPA workload with cache limit = 256MB
WHEN   evaluated with CSE
THEN   peak RSS with CSE <= peak RSS baseline + 256MB
AND    cache.mem_used never exceeds mem_limit
RECORD peak_rss_baseline, peak_rss_cse, max_cache_mem_used
```

### T-P04: DOOP Join Reduction

**Purpose:** Measure join operation reduction for DOOP 8-way rules.

```
GIVEN  DOOP workload with CSE enabled
AND    instrumentation counting total join operations per iteration
WHEN   evaluated with vs without CSE
THEN   joins_per_iteration_cse < joins_per_iteration_baseline
AND    expected: ~78% reduction for 8-way rules (56 -> ~12 joins)
RECORD joins_baseline, joins_cse, reduction_pct
```

---

## 7. Test Infrastructure

### 7.1 Test File Layout

```
tests/
  test_mat_cache.c         -- T-U01 through T-U05 (cache data structure)
  test_delta_mode.c        -- T-U06 through T-U09 (delta_mode ops)
  test_multipass.c          -- T-U10 through T-U12 (multi-pass logic)
  test_cse_cspa.c           -- T-I01, T-I02, T-I05, T-E02, T-E03
  test_cse_doop.c           -- T-I03, T-I04
  test_cse_regression.c     -- T-R01 through T-R04
  test_cse_edge.c           -- T-E01, T-E04, T-E05
```

### 7.2 Baseline Oracle

All integration and regression tests compare against a **baseline oracle**: the existing
evaluation without CSE. The test harness:

1. Evaluates the program **without** CSE (all delta_mode = WL_DELTA_AUTO).
2. Records tuple counts per relation.
3. Evaluates the program **with** CSE multi-pass enabled.
4. Compares tuple counts: must be identical.

```c
/* Pseudocode for oracle comparison */
static int
run_oracle_comparison(const char *dl_file, const char *data_dir)
{
    /* Run baseline */
    wl_session_t *baseline = create_and_evaluate(dl_file, data_dir, false);
    uint32_t *baseline_counts = collect_tuple_counts(baseline);

    /* Run with CSE */
    wl_session_t *cse = create_and_evaluate(dl_file, data_dir, true);
    uint32_t *cse_counts = collect_tuple_counts(cse);

    /* Compare */
    for (int i = 0; i < num_relations; i++) {
        TEST(baseline_counts[i] == cse_counts[i],
             "relation %s: baseline=%u cse=%u",
             relation_names[i], baseline_counts[i], cse_counts[i]);
    }
    return 0;
}
```

### 7.3 Performance Test Setup

Performance tests use the existing benchmark binary:

```bash
# Baseline (without CSE)
./build/bench/bench_flowlog --workload cspa \
    --data bench/data/graph_10.csv \
    --iterations 5 --output baseline.json

# With CSE
./build/bench/bench_flowlog --workload cspa \
    --data bench/data/graph_10.csv \
    --cse-enabled \
    --iterations 5 --output cse.json

# Compare
python3 bench/compare.py baseline.json cse.json
```

Metrics captured per run:
- Wall time (min/median/max over 5 iterations)
- Iteration count (fixed-point iterations)
- Peak RSS (via `/usr/bin/time -v` or `getrusage`)
- Join operation count (via instrumentation counter)

### 7.4 Registration in meson.build

```meson
# In tests/meson.build, add:
test('mat_cache', executable('test_mat_cache',
    'test_mat_cache.c', ir_src,
    dependencies: wirelog_dep))

test('delta_mode', executable('test_delta_mode',
    'test_delta_mode.c', ir_src,
    dependencies: wirelog_dep))

test('multipass', executable('test_multipass',
    'test_multipass.c', ir_src,
    dependencies: wirelog_dep))

test('cse_cspa', executable('test_cse_cspa',
    'test_cse_cspa.c', ir_src,
    dependencies: wirelog_dep))

test('cse_doop', executable('test_cse_doop',
    'test_cse_doop.c', ir_src,
    dependencies: wirelog_dep))

test('cse_regression', executable('test_cse_regression',
    'test_cse_regression.c', ir_src,
    dependencies: wirelog_dep))

test('cse_edge', executable('test_cse_edge',
    'test_cse_edge.c', ir_src,
    dependencies: wirelog_dep))
```

---

## 8. Test Priority and Sequencing

| Priority | Tests | Rationale |
|----------|-------|-----------|
| P0 (blocking) | T-U06, T-U07, T-U08, T-U09 | Delta mode ops are foundation |
| P0 (blocking) | T-R03 | All 15 workloads must pass |
| P1 (critical) | T-U01-T-U05 | Cache correctness gates CSE |
| P1 (critical) | T-U10-T-U12 | Multi-pass logic correctness |
| P1 (critical) | T-P02 | Must not regress (critical gate) |
| P2 (important) | T-I01-T-I05 | Integration with real workloads |
| P2 (important) | T-E01-T-E05 | Edge cases for robustness |
| P3 (measure) | T-P01, T-P03, T-P04 | Performance measurement |
| P3 (measure) | T-R01, T-R02, T-R04 | Regression isolation |

**Implementation order:**
1. P0 tests first (delta_mode ops + full regression suite)
2. P1 tests (cache + multi-pass + performance gate)
3. P2 tests (integration + edge cases)
4. P3 tests (measurement + fine-grained regression)
