# wirelog Benchmark Baseline — 2026-03-07

**Branch:** `next/pure-c11`
**Backend:** Columnar nanoarrow (Phase 2C, DD removed)
**Host:** macOS 25.3.0 / Apple Silicon
**Binary:** `./build/bench/bench_flowlog`
**Run date:** 2026-03-07
**Repeats:** 3 (min/median/max reported), except noted workloads

---

## Data Sets

| Dataset | Path | Total Input Rows |
|---|---|---|
| graph_10.csv | bench/data/graph_10.csv | 9 edges |
| graph_10_weighted.csv | bench/data/graph_10_weighted.csv | 9 weighted edges |
| andersen/ | bench/data/andersen/ | 140 (addressOf+assign+load+store) |
| dyck/ | bench/data/dyck/ | 136 (open1+close1+open2+close2) |
| cspa/ | bench/data/cspa/ | 199 (assign 179 + dereference 20) |
| csda/ | bench/data/csda/ | 303 (nullEdge 15 + edge 288) |
| galen/ | bench/data/galen/ | 246 (c+inputP+inputQ+rc+s+u) |
| polonius/ | bench/data/polonius/ | 197 (subset_base+var_defined/used_at+cfg_edge) |
| ddisasm/ | bench/data/ddisasm/ | 412 (instruction+next+nop+jumps+calls) |
| crdt/ | bench/data/crdt/ | 259,778 (Insert 182,315 + Remove 77,463) |
| doop/ | bench/data/doop/ | 4,178,411 (34 CSV files, zxing) |

---

## Per-Workload Results

Output columns: `workload | input_rows | min_ms | median_ms | max_ms | peak_rss_kb | output_tuples | status`

### Simple Graph Workloads (graph_10.csv: 10 nodes, 9 edges)

| Workload | Input | Min ms | Median ms | Max ms | Peak RSS KB | Tuples | Status |
|---|---|---|---|---|---|---|---|
| TC (Transitive Closure) | 9 edges | 0.1 | 0.1 | 0.3 | 1,712 | 45 | OK |
| Reach (Reachability) | 9 edges | 0.0 | 0.1 | 0.1 | 1,680 | 10 | OK |
| CC (Connected Components) | 9 edges | 0.0 | 0.0 | 0.1 | 1,664 | 10 | OK |
| SSSP (Shortest Path) | 9 w.edges | 0.0 | 0.0 | 0.1 | 1,680 | 1 | OK* |
| SG (Same Generation) | 9 edges | 0.0 | 0.1 | 0.1 | 1,712 | 0 | OK† |
| Bipartite | 9 edges | 0.1 | 0.1 | 0.1 | 1,744 | 10 | OK |

**\* SSSP anomaly**: Only 1 tuple output (`dist(1,0)` seed only). The `min()` aggregate in a
recursive rule (`dist(y, min(d+w)) :- dist(x,d), wedge(x,y,w)`) does not propagate correctly
under the current semi-naive evaluator. Expected: 10 distance tuples for a 10-node chain.
This is a correctness issue with aggregation in recursive strata.

**† SG zero output**: The graph_10 dataset is a 1→2→...→10 chain (no branching parent).
SG requires a shared parent `edge(p,x), edge(p,y)` — none exist in a linear chain. Zero is correct.

### Specialty / Domain Workloads

| Workload | Input | Min ms | Median ms | Max ms | Peak RSS KB | Tuples | Status |
|---|---|---|---|---|---|---|---|
| Andersen (pointer) | 140 facts | 0.2 | 0.2 | 0.3 | 2,048 | 112 | OK |
| Dyck-2 (reachability) | 136 facts | 3.7 | 3.9 | 3.9 | 3,232 | 1,656 | OK |
| **CSPA (context-sensitive points-to)** | **199 facts** | **4,422.1** | **4,602.0** | **4,643.7** | **4,670,592** | **20,381** | **OK (SLOW)** |
| CSDA | 303 facts | 7.1 | 7.1 | 7.4 | 2,752 | 2,986 | OK |
| Galen (ontology) | 246 facts | 11.4 | 11.5 | 11.5 | 3,328 | 2,187 | OK |
| Polonius (borrow check) | 197 facts | 2.7 | 2.7 | 3.1 | 2,848 | 1,999 | OK |
| DDISASM (disassembly) | 412 facts | 1.7 | 1.8 | 1.9 | 2,704 | 900 | OK |

### Large Dataset Workloads

| Workload | Input | Min ms | Median ms | Max ms | Peak RSS KB | Tuples | Status |
|---|---|---|---|---|---|---|---|
| **CRDT (260K rows)** | 259,778 facts | 126,295 | 126,295 | 126,295 | 629,248 | 1,962,161 | **OK (SLOW)** |
| DOOP (4.2M rows) | 4,178,411 facts | — | — | — | — | — | **DNF** |

CRDT ran with `--repeat 1` (single measurement). 126 seconds wall time, 614 MB peak RSS.
DOOP terminated after ~2.5 minutes with no output; `.input` loading of 34 CSVs (4.2M rows) via
`session_load_input_files()` is the bottleneck — estimated multi-hour runtime at this scale.

---

## CSPA Deep-Profile Analysis

CSPA is the critical outlier: **4.4 seconds** for **199 input facts**, **4.46 GB peak RSS**.

### Rules Structure
```
valueFlow(y, x) :- assign(y, x).           -- base (EDB → IDB)
valueFlow(x, x) :- assign(x, _).           -- base
valueFlow(x, x) :- assign(_, x).           -- base
memoryAlias(x, x) :- assign(_, x).         -- base
memoryAlias(x, x) :- assign(x, _).         -- base
valueFlow(x, y) :- valueFlow(x, z), valueFlow(z, y).          -- TC over assign
valueFlow(x, y) :- assign(x, z), memoryAlias(z, y).           -- cross
memoryAlias(x, w) :- dereference(y, x), valueAlias(y, z), dereference(z, w).
valueAlias(x, y) :- valueFlow(z, x), valueFlow(z, y).
valueAlias(x, y) :- valueFlow(z, x), memoryAlias(z, w), valueFlow(w, y).
```

3 mutually-recursive IDB relations: **valueFlow**, **memoryAlias**, **valueAlias**.

### Per-Iteration Cost Structure (estimated from code)

Each iteration of the semi-naive fixed-point loop does:

1. **Register deltas** from previous iteration (~O(1))
2. **Snapshot pre-evaluation state**: `malloc + memcpy` for all 3 relations = O(N) bytes copied
3. **Evaluate 3 IDB relations**: hash-join based rule evaluation per relation
4. **CONSOLIDATE** (sort+dedup) ALL IDB relations: `qsort_r(all_accumulated_rows)` = O(N log N) × 3
5. **Compute delta** via sorted merge walk = O(N + delta)
6. **Free snapshots**

### Estimated Iteration Count

Input: 179 assign + 20 dereference = 199 facts
Output: 20,381 final tuples (valueFlow + memoryAlias + valueAlias)

The CSPA rules form a transitive closure over assign with cross-dependency through memoryAlias.
For 179 assign edges with ~100 unique node IDs, TC convergence typically requires O(diameter) iterations.
With mutually recursive rules and incomplete delta expansion (H2), estimated **100–300 iterations**.

Estimated per-iteration cost breakdown:
- Early iterations (small N ≈ 200 rows): ~1ms each (dominated by join)
- Late iterations (large N ≈ 20K rows): ~20-50ms each (CONSOLIDATE sort dominates)
- **Sort cost at convergence**: `qsort_r(20,381 rows × 3 relations)` ≈ 18ms per final iteration

### Memory Profile

- Peak RSS: **4,670,592 KB ≈ 4.46 GB** for 20,381 output tuples at 16 bytes/row ≈ 320 KB final state
- Memory amplification factor: **~14,000×** over final output size
- Sources of amplification:
  - `old_data` snapshots: 3 × copy of accumulated rows per iteration
  - Hash table for join: `next_pow2(N × 2)` buckets allocated per join op
  - Intermediate join results before CONSOLIDATE: can be much larger than final (O(N²) before dedup)
  - Arena for per-iteration temporaries
- Memory is not reclaimed between iterations (arena grows monotonically)

---

## CRDT Deep-Profile Analysis

CRDT workload has **259,778 input rows** (Insert 182,315 + Remove 77,463).
**Result**: 126,295ms (126 seconds), 614 MB peak RSS, 1,962,161 output tuples (`--repeat 1`).

### Rules Structure (excerpt)

```
laterChild(pc,pn,c2,n2) :- insert(c1,n1,pc,pn), insert(c2,n2,pc,pn),
                            c1*10+n1 > c2*10+n2.
```

This is a **self-join of the 182K insert relation** grouped by `(pc, pn)`.
With 182K rows, even a hash-join partitioned by parent produces O(K²) pairs
per parent where K = children per parent node. For realistic tree data with
branching factor B, this is O(N × B) pairs total — but with B potentially in
the thousands for root-level nodes, the cross-join is infeasible.

**Actual result** (`--repeat 1`): 126,295ms, 614 MB peak RSS, 1,962,161 output tuples. CRDT
completes but is very slow. Hash-join partitions `laterChild` by `(parent_ctr, parent_node)`
so only siblings per parent are paired — the join is feasible but produces many tuples. With
1.96M output tuples, CONSOLIDATE on the full accumulated relation per iteration (same H1 pathology
as CSPA) dominates the 126-second runtime.

---

## Summary Table

| Category | Workloads | Median ms | Peak RSS | Notes |
|---|---|---|---|---|
| Fast (sub-ms) | TC, Reach, CC, SSSP, SG, Bipartite, Andersen, DDISASM | <2ms | <3 MB | Trivial at graph_10 scale |
| Medium | Dyck, Polonius, CSDA, Galen | 2–12ms | 2–4 MB | Converge in few iterations |
| **Critical bottleneck** | **CSPA** | **4,602ms** | **4,460 MB** | Semi-naive sort-per-iteration + incomplete delta |
| Large data slow | **CRDT** | **126,295ms** | **614 MB** | 1.96M output tuples; `laterChild` cross-join on 182K rows |
| Large data DNF | DOOP | DNF | — | 4.2M row `.input` loading; multi-hour runtime estimated |

**Key findings**:

1. **CSPA** (199 facts → 4.6s, 4.46 GB): Full O(N log N) re-sort every iteration + incomplete delta expansion causes a **648× slowdown** vs CSDA with similar input size.

2. **CRDT** (260K facts → 126s, 614 MB, 1.96M tuples): Completes but takes 126 seconds due to the `laterChild` self-join on 182K insert rows. Hash-join partitioned by parent reduces pairs but remains expensive at this scale.

3. **DOOP** (4.2M facts): Does not complete in a reasonable timeframe. Requires streaming or incremental `.input` loading strategy.
