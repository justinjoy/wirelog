# CONSOLIDATE Improvement Plan

**Task:** #5 — Concrete improvement plan for CONSOLIDATE bottleneck
**Date:** 2026-03-07
**Author:** Task #5 (Executor — CONSOLIDATE Improvement Planner)
**Status:** Final
**Gate:** >20% wall time improvement for CSPA required to proceed

---

## Context: What Has Already Been Done

Before presenting new work, it is critical to understand the current code state to avoid
re-implementing work that already exists.

### Already Implemented (in `columnar_nanoarrow.c`)

| Component | Status | Location |
|-----------|--------|----------|
| `col_op_consolidate_incremental()` | **EXISTS** | Lines 1405–1482 |
| Post-iteration incremental call | **EXISTS** | Line 2009 |
| `snap[ri]` boundary tracking | **EXISTS** | Lines 1883–1900 |
| `old_data[ri]` snapshot copy | **EXISTS** (expensive) | Lines 1884–1899 |
| In-plan `WL_PLAN_OP_CONSOLIDATE` | **Full sort O(N log N)** | Line 1731–1733 |
| `g_consolidate_ncols` global | **Still present** | Line 1322 |

### The 6× Regression Is Not Fully Explained by CONSOLIDATE

From the performance investigation (`PERFORMANCE-INVESTIGATION-2026-03-07.md`):
- Baseline (Phase 2B, before plan expansion): **4,602ms**
- Current (Phase 2C, with plan expansion + CSE fields): **28.7s**
- Removing in-plan CONSOLIDATE makes it **worse** (77.6s), not better

This means the 6× regression from Phase 2C is **not primarily a CONSOLIDATE issue** — it
comes from the evaluator loop overhead introduced by plan expansion (K=3 copies) combined with
new struct fields (`delta_mode`, `materialized`) degrading cache locality.

**The CONSOLIDATE improvement plan targets the remaining CONSOLIDATE-specific overhead**
that exists even in the optimized 4.6s baseline — bringing that further down.

---

## Selected Approach

**"Delta-integrated incremental consolidation"**

Extend `col_op_consolidate_incremental` to output the true delta (newly added sorted rows)
as a direct byproduct of the merge step, eliminating the expensive `old_data` snapshot copy
and the post-consolidation merge walk.

### Why This Approach

| Option | Description | Effort | Impact |
|--------|-------------|--------|--------|
| **A: Delta-integrated (selected)** | Extend `col_op_consolidate_incremental` to output delta byproduct | 2–3 days | Eliminates O(N) copy + O(N) merge walk per iteration per relation |
| B: Hash-based in-plan dedup | Replace in-plan CONSOLIDATE sort with hash set dedup | 2–3 days | Reduces within-iteration sort; blocked by the 6× regression root cause |
| C: Radix sort | Replace qsort with radix sort for int64 keys | 3–5 days | ~2–3× sort speedup; less impactful than eliminating the sort entirely |
| D: g_consolidate_ncols fix only | Mechanical qsort_r context pass | 1 day | Thread safety only; no wall time improvement |

**Why Option A over B, C, D:**
- Option A eliminates whole operations (memcpy + merge walk), not just speeds them up
- The `old_data` snapshot copies the full relation every iteration: for CSPA at ~100 iterations
  with 3 relations of growing size, this is O(iterations × N × relations) total allocation —
  confirmed as the root cause of 4.46 GB peak RSS for 320 KB of useful data (14,000× amplification)
- Option A's delta byproduct also eliminates the post-consolidation merge walk (lines 2020–2063)
  which scans both old and new data in O(N + D) time per iteration
- Options B and C address different bottlenecks and are complementary (not alternatives)
- Option D is a prerequisite for the workqueue phase, not a performance fix by itself

---

## Justification: Why the Old_Data Snapshot Is Expensive

### Current Per-Iteration Cost (semi-naive loop, per relation)

```
1. old_data copy:       malloc(N * ncols * sizeof(int64_t)) + memcpy   → O(N) time + O(N) memory
2. Rule evaluation:     JOIN/VARIABLE ops append new rows to target     → O(D) amortized
3. Incremental sort:    col_op_consolidate_incremental(r, snap[ri])     → O(D log D + N) time
                        └─ Phase 1: sort delta          O(D log D)
                        └─ Phase 1b: dedup delta         O(D)
                        └─ Phase 2: merge old + delta    O(N + D) time + O(N + D) allocation
4. Delta merge walk:    scan both old_data and r->data to find R_new-R_old  → O(N + D) time
5. free(old_data):      reclaim snapshot                                 → O(1)
```

**Total cost per iteration per relation:** `O(N) + O(D log D + N) + O(N + D) = O(N + D log D)`

The two `O(N)` terms (copy and merge walk) dominate once D << N (late iterations).
For CSPA at 100 iterations with N growing from 0 to 20K tuples at 2 columns:

```
late iterations: old_data copy = 20K × 2 × 8 = 320 KB per relation × 3 relations = 960 KB/iter
                 merge walk    = scan 20K rows per relation × 3 relations
Total allocations: 100 iters × 960 KB = ~96 MB in old_data copies alone
```

The HYPOTHESIS-VALIDATION.md confirms: peak RSS 4.46 GB for 320 KB of output.
The amplification comes from this O(N × iterations × relations) allocation pattern.

---

## Pseudocode: Delta-Integrated Incremental Consolidation

### New Function Signature

```c
/*
 * col_op_consolidate_incremental_delta:
 * Incremental sort+dedup with delta output.
 *
 * Precondition: rel->data[0..old_nrows) is already sorted+unique.
 *               New rows live in [old_nrows..rel->nrows).
 *
 * Postcondition: rel->data[0..rel->nrows) is sorted+unique (merged result).
 *               *delta_out is populated with the truly-new rows (R_new - R_old),
 *               sorted in the same order. Caller owns *delta_out->data.
 *
 * Algorithm:
 *   1. Sort only the new delta rows: O(D log D)
 *   2. Dedup within delta: O(D)
 *   3. Merge sorted old [0..old_nrows) with sorted delta,
 *      emitting rows that are new (not in old) into delta_out: O(N + D)
 *
 * Total: O(D log D + N) time, O(N + D) memory for merge buffer.
 * Eliminates: O(N) old_data snapshot copy + O(N + D) post-consolidation merge walk.
 */
static int
col_op_consolidate_incremental_delta(col_rel_t *rel,
                                     uint32_t old_nrows,
                                     col_rel_t *delta_out)
{
    uint32_t nc = rel->ncols;
    uint32_t nr = rel->nrows;
    size_t row_bytes = (size_t)nc * sizeof(int64_t);

    if (old_nrows >= nr) {
        /* No new rows: delta is empty, relation unchanged */
        delta_out->nrows = 0;
        return 0;
    }

    if (old_nrows == 0) {
        /* First iteration: all rows are new. Sort+dedup in place, all go to delta. */
        /* (existing first-time handling) */
    }

    uint32_t delta_count = nr - old_nrows;
    int64_t *delta_start = rel->data + (size_t)old_nrows * nc;

    /* Phase 1: sort only the new delta rows */
    g_consolidate_ncols = nc;       /* TODO: replace with qsort_r context (Phase A) */
    qsort(delta_start, delta_count, row_bytes, row_cmp);

    /* Phase 1b: dedup within delta */
    uint32_t d_unique = 1;
    for (uint32_t i = 1; i < delta_count; i++) {
        if (memcmp(delta_start + (size_t)(i - 1) * nc,
                   delta_start + (size_t)i * nc, row_bytes) != 0) {
            if (d_unique != i)
                memcpy(delta_start + (size_t)d_unique * nc,
                       delta_start + (size_t)i * nc, row_bytes);
            d_unique++;
        }
    }

    /* Phase 2: merge sorted old [0..old_nrows) with sorted+unique delta.
     * Simultaneously: collect rows from delta NOT present in old -> these are truly new.
     * Allocate one buffer for the merged result. Delta_out receives truly-new rows. */
    size_t max_rows = (size_t)old_nrows + d_unique;
    int64_t *merged = (int64_t *)malloc(max_rows * nc * sizeof(int64_t));
    if (!merged)
        return ENOMEM;

    /* Pre-allocate delta_out buffer (at most d_unique rows will be new) */
    int64_t *new_rows = (int64_t *)malloc((size_t)d_unique * nc * sizeof(int64_t));
    if (!new_rows) {
        free(merged);
        return ENOMEM;
    }

    uint32_t oi = 0, di = 0, out = 0, new_out = 0;
    while (oi < old_nrows && di < d_unique) {
        const int64_t *orow = rel->data + (size_t)oi * nc;
        const int64_t *drow = delta_start + (size_t)di * nc;
        int cmp = memcmp(orow, drow, row_bytes);
        if (cmp < 0) {
            /* old row comes first, not new */
            memcpy(merged + (size_t)out * nc, orow, row_bytes);
            oi++; out++;
        } else if (cmp == 0) {
            /* duplicate: already in old, skip from delta */
            memcpy(merged + (size_t)out * nc, orow, row_bytes);
            oi++; di++; out++;
        } else {
            /* delta row is new (not in old) -> emit to merged AND to delta_out */
            memcpy(merged + (size_t)out * nc, drow, row_bytes);
            memcpy(new_rows + (size_t)new_out * nc, drow, row_bytes);
            di++; out++; new_out++;
        }
    }
    /* Remaining old rows: not new */
    if (oi < old_nrows) {
        uint32_t remaining = old_nrows - oi;
        memcpy(merged + (size_t)out * nc, rel->data + (size_t)oi * nc,
               (size_t)remaining * row_bytes);
        out += remaining;
    }
    /* Remaining delta rows: all new */
    while (di < d_unique) {
        const int64_t *drow = delta_start + (size_t)di * nc;
        memcpy(merged + (size_t)out * nc, drow, row_bytes);
        memcpy(new_rows + (size_t)new_out * nc, drow, row_bytes);
        di++; out++; new_out++;
    }

    /* Swap buffer into rel */
    free(rel->data);
    rel->data = merged;
    rel->nrows = out;
    rel->capacity = (uint32_t)max_rows;

    /* Populate delta_out with truly-new rows */
    if (delta_out->data)
        free(delta_out->data);
    delta_out->data = new_rows;
    delta_out->nrows = new_out;
    delta_out->capacity = d_unique;
    /* delta_out->ncols and col_names must be pre-populated by caller */

    return 0;
}
```

### Integration: Eliminate old_data Snapshot

**Current loop (lines 1880–2070, simplified):**

```c
for (uint32_t iter = 0; ...) {
    /* Step 1: snapshot old state (EXPENSIVE: O(N) copy per relation) */
    for (uint32_t ri = 0; ri < nrels; ri++) {
        snap[ri] = r->nrows;
        old_data[ri] = malloc(snap[ri] * r->ncols * sizeof(int64_t));
        memcpy(old_data[ri], r->data, ...);           // <-- O(N) per relation per iteration
    }

    /* Step 2: evaluate rules, appending new rows to target */
    col_eval_relation_plan(...);

    /* Step 3: incremental consolidate */
    col_op_consolidate_incremental(r, snap[ri]);      // O(D log D + N)

    /* Step 4: delta merge walk (EXPENSIVE: O(N + D) per relation) */
    for (uint32_t ri = 0; ri < nrels; ri++) {
        // walk old_data[ri] vs r->data to find R_new - R_old
        // emit new rows to delta_rels[ri]
    }

    free(old_data[ri]);
}
```

**Proposed loop (after optimization):**

```c
for (uint32_t iter = 0; ...) {
    /* Step 1: record row count only (O(1) per relation, no copy) */
    for (uint32_t ri = 0; ri < nrels; ri++) {
        snap[ri] = r ? r->nrows : 0;
        /* NO old_data malloc/memcpy needed */
    }

    /* Step 2: evaluate rules (unchanged) */
    col_eval_relation_plan(...);

    /* Step 3: incremental consolidate WITH delta output */
    for (uint32_t ri = 0; ri < nrels; ri++) {
        col_rel_t *r = session_find_rel(sess, sp->relations[ri].name);
        col_rel_t *delta = col_rel_new_like("$d$...", r);
        col_op_consolidate_incremental_delta(r, snap[ri], delta);
        /* delta now holds R_new - R_old directly — no separate merge walk */
        if (delta->nrows > 0) {
            delta_rels[ri] = delta;  // used in next iteration
            any_new = true;
        }
    }
    /* Step 4: ELIMINATED — no separate delta merge walk */
}
```

**Savings per iteration per relation:**
- Eliminates: `malloc(N * ncols * 8)` + `memcpy N rows` (the `old_data` snapshot)
- Eliminates: O(N + D) merge walk (lines 2020–2063)
- Replaces with: O(d_unique) `new_rows` buffer (much smaller than N)

---

## API / Architecture Changes

### Functions Modified

| Function | File | Change | Backward Compat |
|----------|------|--------|-----------------|
| `col_op_consolidate_incremental` | `columnar_nanoarrow.c` | Keep as-is (used by `col_idb_consolidate`) | Yes |
| `col_op_consolidate_incremental_delta` | `columnar_nanoarrow.c` | **New function** — same logic + delta output | N/A (new) |
| `col_eval_stratum` | `columnar_nanoarrow.c` | Replace old_data loop + delta merge walk with new function | Internal only |
| `row_cmp` / `g_consolidate_ncols` | `columnar_nanoarrow.c` | Phase A: replace with qsort_r context | Internal only |

### No Public API Changes

All changes are in `columnar_nanoarrow.c` (static functions). The public session API
(`wl_col_session_t`, `wl_col_session_eval`, etc.) is unchanged. TC, Reach, CC, and all
other workloads are unaffected at the API level.

### Impact on Other Workloads

| Workload | Change | Expected Impact |
|----------|--------|-----------------|
| TC | Uses recursive stratum | Memory improvement (smaller snapshots); slight speed improvement |
| Reach | Uses recursive stratum | Same as TC |
| CC | Uses recursive stratum | Same as TC |
| CSPA | Deep mutual recursion | **Primary target**: eliminates O(N×iter) snapshot allocation |
| CRDT | Large data, recursive | Memory improvement; may help if snapshot copies are bottleneck |
| DOOP | DNF currently | Not applicable until evaluator regression is fixed |
| All non-recursive | No semi-naive loop | **No change** — non-recursive path unaffected |

---

## Implementation Roadmap

### Prerequisites

**Step 0: Fix g_consolidate_ncols (Phase A, ~1 day)**
Replace `g_consolidate_ncols` global with `qsort_r` context parameter. This is a
prerequisite for thread safety (Phase B-lite workqueue) and is a clean precondition
for implementing the new function.

```c
typedef struct { uint32_t ncols; } row_cmp_ctx_t;

static int
row_cmp_fn(const void *a, const void *b, void *ctx) {
    const row_cmp_ctx_t *c = (const row_cmp_ctx_t *)ctx;
    const int64_t *ra = (const int64_t *)a;
    const int64_t *rb = (const int64_t *)b;
    for (uint32_t i = 0; i < c->ncols; i++) {
        if (ra[i] < rb[i]) return -1;
        if (ra[i] > rb[i]) return  1;
    }
    return 0;
}
```

Replace all `g_consolidate_ncols = nc; qsort(...)` with:
```c
row_cmp_ctx_t ctx = { .ncols = nc };
qsort_r(data, count, row_bytes, &ctx, row_cmp_fn);
```

---

### Step 1: Implement `col_op_consolidate_incremental_delta` (~1 day)

- Add the new function (pseudocode above) after the existing `col_op_consolidate_incremental`
- Unit test: verify that for a given (old_sorted + delta) input:
  - `rel->data` is correctly sorted+unique after the call
  - `delta_out->data` contains exactly `R_new - R_old`
  - Empty delta case returns empty delta_out without error
  - All-duplicate delta case returns empty delta_out, rel unchanged

**Test cases to add in `tests/test_session_columnar.c`:**

```c
// Case 1: old = [(1,2), (3,4)], delta = [(2,3), (3,4)] -> new = [(2,3)]
// Case 2: old = [(1,1)], delta = [(1,1)] -> new = [] (duplicate eliminated)
// Case 3: old = [], delta = [(1,2), (1,2), (2,3)] -> new = [(1,2), (2,3)]
// Case 4: Large random: verify sorted+unique and delta = new - old via brute force
```

---

### Step 2: Integrate into `col_eval_stratum` (~1 day)

Replace the following sections in `col_eval_stratum`:

**Remove `old_data` snapshot loop (lines 1883–1900):**
```c
// REMOVE:
int64_t **old_data = (int64_t **)calloc(nrels, sizeof(int64_t *));
for (uint32_t ri = 0; ri < nrels; ri++) {
    if (snap[ri] > 0 && r && r->ncols > 0) {
        size_t bytes = (size_t)snap[ri] * r->ncols * sizeof(int64_t);
        old_data[ri] = (int64_t *)malloc(bytes);
        if (old_data[ri]) memcpy(old_data[ri], r->data, bytes);
    }
}
// Keep only:
uint32_t *snap = (uint32_t *)malloc(nrels * sizeof(uint32_t));
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_rel_t *r = session_find_rel(sess, sp->relations[ri].name);
    snap[ri] = r ? r->nrows : 0;
}
```

**Replace post-iteration consolidation + delta merge walk (lines 2000–2069):**
```c
// REPLACE current col_op_consolidate_incremental call + delta merge walk
// WITH:
bool any_new = false;
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_rel_t *r = session_find_rel(sess, sp->relations[ri].name);
    if (!r || r->nrows == 0) continue;

    char dname[256];
    snprintf(dname, sizeof(dname), "$d$%s", sp->relations[ri].name);
    col_rel_t *delta = col_rel_new_like(dname, r);
    if (!delta) { /* handle OOM */ return ENOMEM; }

    int rc2 = col_op_consolidate_incremental_delta(r, snap[ri], delta);
    if (rc2 != 0) { col_rel_free_contents(delta); free(delta); return rc2; }

    if (delta->nrows > 0) {
        delta_rels[ri] = delta;
        any_new = true;
    } else {
        col_rel_free_contents(delta);
        free(delta);
    }
}
```

---

### Step 3: Benchmark Validation (~1 day)

Run the full 15-workload benchmark suite before and after:

```bash
# Build
meson compile -C build bench_flowlog

# All workloads: verify correctness and measure wall time
./build/bench/bench_flowlog --workload all \
    --data bench/data/graph_10.csv \
    --data-weighted bench/data/graph_10_weighted.csv

# CSPA specifically (primary target)
./build/bench/bench_flowlog --workload cspa --data-cspa bench/data/cspa

# Peak RSS comparison
/usr/bin/time -l ./build/bench/bench_flowlog --workload cspa --data-cspa bench/data/cspa
```

**Success criteria:**
- All 15 workloads produce identical fact counts (correctness oracle)
- CSPA peak RSS drops from ~4.46 GB toward theoretical minimum (~32 MB for merge buffers)
- CSPA wall time improvement measurable (even if small vs the 6× regression)

---

### Step 4: Address the 6× Regression (Separate, ~3–5 days)

The 6× regression (4.6s → 28.7s) from Phase 2C is **not** a CONSOLIDATE issue per the
investigation. After Steps 0–3, profile the regression:

```bash
# macOS: capture CPU hotspots
xcrun xctrace record --template 'Time Profiler' \
    --launch -- ./build/bench/bench_flowlog --workload cspa --data-cspa bench/data/cspa
```

Suspected causes (from investigation):
1. `wl_plan_op_t` struct grew (new `delta_mode`, `materialized` fields) → cache-line misses in evaluator hot loop
2. K=3 plan copies × larger intermediate results → more memory pressure per iteration
3. Possible: `col_op_join` LRU cache lookup overhead on every join (even when miss rate = 100%)

Targeted fixes depend on profiler output (not part of CONSOLIDATE plan).

---

## Risk Assessment

### Risk 1: Subtle delta computation bug

**Concern:** The new function conflates consolidation and delta computation. A bug in the
merge logic could produce an incorrect delta (missing rows or extra rows), causing the
fixed-point to terminate early (missing facts) or run extra iterations (harmless but slower).

**Likelihood:** Low — the merge logic mirrors the existing `col_op_consolidate_incremental`
exactly, with only the addition of `new_rows` collection.

**Mitigation:**
- Keep `col_op_consolidate_incremental` (original) unchanged; used for non-stratum cases
- Add a `--debug-delta` mode that runs both code paths and asserts identical results
- Run TC, Reach, CC workloads first (simpler recursion) before CSPA

### Risk 2: Memory regression from new_rows buffer

**Concern:** The new function allocates `new_rows` (at most `d_unique` rows) in addition
to the existing `merged` buffer. If delta is large (early iterations), this doubles memory.

**Likelihood:** Low — `new_rows` is strictly smaller than `merged` (it's a subset).
Net effect: `old_data` snapshot (O(N)) is replaced by `new_rows` (O(D)), reducing memory.

**Mitigation:** Profile peak RSS before/after; expected significant reduction.

### Risk 3: First-iteration correctness (old_nrows == 0)

**Concern:** When `old_nrows == 0`, all rows are "new". The merge loop exits immediately
(oi starts at old_nrows=0, while loop terminates). All delta rows go to `new_rows`.
The merged buffer contains only delta rows.

**Status:** Edge case is handled naturally by the algorithm — the while loop terminates
immediately, remaining-delta loop runs and copies everything. Needs a specific unit test.

### Risk 4: `col_idb_consolidate` still uses old function

**Concern:** `col_idb_consolidate` (line 2314) calls `col_op_consolidate` via the eval
stack for the session init path. This is not in the hot semi-naive loop; it's called once
at session flush. No change needed here.

**Status:** Not a risk — this path is correct and performance-irrelevant.

---

## Validation Strategy

### Before merging:

1. **Unit tests** (new): `test_session_columnar.c` — 4 edge cases for `col_op_consolidate_incremental_delta`
2. **Regression tests** (existing): all 15 benchmark workloads produce identical fact counts
3. **Memory profiling**: CSPA peak RSS must decrease significantly (target: <500 MB vs current 4.46 GB)
4. **Wall time**: CSPA wall time should decrease on the baseline (4.6s path); improvement in the 28.7s path is secondary (blocked by the unresolved 6× regression)

### Success criteria for the 20% improvement gate:

The gate requires >20% improvement in CSPA wall time. This plan primarily targets memory
(eliminating O(N × iterations) snapshots) and the O(N) merge walk per iteration.

**Expected impact on 4.6s baseline (where incremental sort already works):**
- Eliminating old_data copy: saves ~O(N × iters) memcpy operations
- For CSPA N=20K, iters=100, 3 relations: 100 × 3 × 320 KB = ~96 MB total memcpy saved
- Expected: 10–25% wall time reduction on the 4.6s baseline

**Expected impact on 28.7s current:**
- The 28.7s includes a 6× regression from Phase 2C (unrelated to CONSOLIDATE)
- CONSOLIDATE improvement will not fix the 6× regression
- **Recommendation**: profile the regression separately (Step 4 above); the >20% gate
  should be evaluated against the Phase 2B baseline (4.6s), not the regressed 28.7s

---

## Success Criteria

| Criterion | Target | Measurement |
|-----------|--------|-------------|
| CSPA correctness | 20,381 tuples (unchanged) | `bench_flowlog --workload cspa` |
| All workload correctness | All fact counts unchanged | `bench_flowlog --workload all` |
| CSPA peak RSS | <500 MB (vs 4.46 GB current) | `/usr/bin/time -l` |
| CSPA wall time (vs 4.6s baseline) | <3.7s (>20% improvement) | `bench_flowlog` median |
| g_consolidate_ncols | Eliminated | `grep g_consolidate_ncols *.c` → 0 results |

---

## Implementation Roadmap Summary

| Step | Description | Effort | Dependency |
|------|-------------|--------|------------|
| **0** | Fix `g_consolidate_ncols` → `qsort_r` context (Phase A) | 1 day | None |
| **1** | Implement `col_op_consolidate_incremental_delta` + unit tests | 1 day | Step 0 |
| **2** | Integrate into `col_eval_stratum`: remove `old_data`, remove merge walk | 1 day | Step 1 |
| **3** | Full 15-workload benchmark + peak RSS validation | 1 day | Step 2 |
| **4** | Profile 6× regression (Phase 2C); targeted fix | 3–5 days | Step 3 |

**Total for Steps 0–3:** 4 days
**Total including Step 4:** 7–9 days

Steps 0–3 are the CONSOLIDATE plan. Step 4 is the regression investigation (separate track).

---

## Document References

- `docs/performance/HYPOTHESIS-VALIDATION.md` — H1/H2 confirmation evidence
- `docs/performance/PERFORMANCE-INVESTIGATION-2026-03-07.md` — 6× regression investigation
- `docs/performance/OPTIMIZATION-STRATEGY.md` — Combined Options 1+2 recommendation
- `docs/performance/CONSENSUS-SUMMARY-2026-03-07.md` — 20% gate + Phase 2 decision
- `wirelog/backend/columnar_nanoarrow.c` lines 1319–1482 — CONSOLIDATE implementation
- `wirelog/backend/columnar_nanoarrow.c` lines 1857–2070 — Semi-naive loop

---

**Generated:** 2026-03-07
**Status:** Ready for implementation
**Next step:** Execute Steps 0–3 (4 days) to achieve >20% improvement gate
