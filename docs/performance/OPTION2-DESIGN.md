# Option 2 Redesign: CSE + Progressive Materialization for Multi-Way Delta Expansion

**Date:** 2026-03-07
**Status:** Design Phase (US-001)
**Predecessor:** Phase 2C Option 2 (reverted commit 56c9b32 due to 8.4x regression)

---

## Executive Summary

The original Option 2 (plan rewriting for K-atom multi-way delta expansion) was reverted because
naively emitting K copies of each K-atom recursive rule caused an explosion in plan size and
redundant join work. This redesign introduces **Common Subexpression Elimination (CSE)** via
**Progressive Materialization**: intermediate join results shared across multiple delta
permutations are computed once and cached, eliminating the redundant work that caused the
regression.

**Key insight:** For a K-atom rule like `R :- A, B, C, D, E, F, G, H`, the original approach
creates 8 full plan copies. With CSE, the shared prefix `A x B x ... x F` (the first K-2 atoms
joined) is materialized once per iteration, and each delta permutation only recomputes the
portion that differs. This reduces effective work from O(K) full join chains to O(K) lookups
against a single materialized intermediate.

---

## 1. Algorithm: K-Way Join Delta Expansion with CSE

### 1.1 Problem Recap

Semi-naive evaluation requires that for a K-atom recursive rule:

```
R(out) :- A1(x1), A2(x2), ..., AK(xK)
```

Every iteration must compute the union of K delta variants:

```
Pass i:  A1_full x A2_full x ... x delta(Ai) x ... x AK_full
```

for i = 1..K. The original implementation emitted K copies of the entire plan. For CSPA's
3-way joins this is manageable (3 copies), but for DOOP's 8-way joins it creates 8 copies,
each performing 7 joins -- leading to 56 total join operations per iteration instead of 7.

### 1.2 CSE Strategy

Instead of K independent plan copies, we use a **single evaluation loop** that:

1. **Materializes shared sub-joins** that appear identically across multiple delta variants.
2. **Iterates delta positions** within the evaluator, reusing materialized intermediates.
3. **Unions results** from all K passes before appending to the target relation.

### 1.3 Algorithm Pseudocode

```
function eval_rule_with_cse(rule, session, materialization_cache):
    K = number of body atoms in rule
    if K <= 2:
        return eval_rule_original(rule, session)  // 2-atom rules already correct

    result = empty_relation()

    // Phase 1: Materialize prefix joins using full relations
    // For atoms [A1, A2, ..., AK], compute:
    //   M[1]    = A1_full
    //   M[1..2] = A1_full x A2_full
    //   M[1..j] = M[1..j-1] x Aj_full   for j = 2..K-1
    //
    // These are reused across delta passes.

    prefixes = []   // prefixes[j] = A1_full x ... x A_{j+1}_full
    cur = lookup_full(atoms[0])
    prefixes[0] = cur
    for j in 1..K-2:
        cur = join(cur, lookup_full(atoms[j]))
        prefixes[j] = cur
        if should_materialize(cur, memory_budget):
            materialization_cache.store(rule_id, j, cur)

    // Phase 2: For each delta position i, compute the delta variant
    for i in 0..K-1:
        delta_i = lookup_delta(atoms[i])
        if delta_i is empty:
            continue  // skip: no new facts for this atom

        // Build the delta variant using prefix/suffix reuse:
        //   Pass i: A1_full x ... x delta(Ai) x ... x AK_full
        //
        // Left prefix:  prefixes[i-1] = A1_full x ... x A_{i-1}_full
        // Delta atom:   delta(Ai)
        // Right suffix:  A_{i+1}_full x ... x AK_full (computed on the fly)

        if i == 0:
            variant = delta_i
        else:
            variant = join(prefixes[i-1], delta_i)

        for j in i+1..K-1:
            variant = join(variant, lookup_full(atoms[j]))

        result = union(result, variant)

    return result
```

### 1.4 Optimization: Suffix Materialization

For large K (e.g., DOOP 8-way), we can also materialize **suffix joins**:

```
S[j..K] = A_j_full x A_{j+1}_full x ... x AK_full
```

This allows delta position i to be computed as:

```
Pass i:  prefixes[i-1] x delta(Ai) x suffixes[i+1]
```

reducing each pass to exactly 2 joins regardless of K. However, this doubles materialization
memory. The cost model (Section 5) determines when suffix materialization is worthwhile.

---

## 2. Data Structures

### 2.1 Materialized Join Cache

```c
/**
 * col_materialized_join_t:
 *
 * Cached intermediate join result for CSE across delta permutations.
 * Stored per-rule, per-iteration. Invalidated when any participating
 * relation's delta is non-empty (the materialized result is stale).
 *
 * @rule_idx:      Index of the rule within the stratum plan.
 * @prefix_depth:  Number of atoms joined (e.g., 3 means A1 x A2 x A3).
 * @result:        The materialized join result (owned).
 * @atom_versions: Array of relation nrows at materialization time.
 *                 Used for staleness detection.
 * @ncols:         Column count of the materialized result.
 */
typedef struct {
    uint32_t rule_idx;
    uint32_t prefix_depth;
    col_rel_t *result;
    uint32_t *atom_versions;
    uint32_t atom_count;
    uint32_t ncols;
} col_materialized_join_t;

/**
 * col_mat_cache_t:
 *
 * Per-stratum materialization cache. Holds materialized prefix joins
 * for all rules in the current recursive stratum.
 *
 * @entries:    Array of materialized join entries.
 * @count:     Number of entries.
 * @capacity:  Allocated capacity.
 * @mem_used:  Total bytes used by materialized results.
 * @mem_limit: Maximum bytes allowed for materialization.
 */
typedef struct {
    col_materialized_join_t *entries;
    uint32_t count;
    uint32_t capacity;
    size_t mem_used;
    size_t mem_limit;
} col_mat_cache_t;
```

### 2.2 Extended Plan Op: delta_mode Enum

The existing `wl_delta_mode_t` enum (already in `exec_plan.h:153-157`) is sufficient:

```c
typedef enum {
    WL_DELTA_AUTO = 0,        /* heuristic (current default) */
    WL_DELTA_FORCE_DELTA = 1, /* force delta relation */
    WL_DELTA_FORCE_FULL = 2,  /* force full relation */
} wl_delta_mode_t;
```

The `wl_plan_op_t.delta_mode` field (already at `exec_plan.h:237`) is already present.
No structural changes needed to exec_plan.h.

### 2.3 Multi-Pass Rule Descriptor

Instead of rewriting the plan, we add a **runtime descriptor** that the evaluator
consults during recursive stratum evaluation:

```c
/**
 * col_multipass_rule_t:
 *
 * Describes a K-atom recursive rule that requires multi-pass delta
 * expansion. The evaluator uses this to iterate delta positions
 * at runtime rather than duplicating plan ops at compile time.
 *
 * @relation_idx:  Index into stratum's relations[] array.
 * @atom_count:    Number of body atoms (K). Only rules with K >= 3 need this.
 * @atom_ops:      Array of (op_start, op_end) pairs indexing into the
 *                 relation plan's ops[] array. atom_ops[i] identifies the
 *                 VARIABLE + subsequent JOINs for atom i.
 * @join_op_indices: Indices of JOIN ops in the relation plan. Length = K-1.
 *                   join_op_indices[j] is the ops[] index of the join that
 *                   incorporates atom j+1.
 * @var_op_index:    Index of the initial VARIABLE op in ops[].
 */
typedef struct {
    uint32_t relation_idx;
    uint32_t atom_count;

    struct {
        uint32_t var_op_idx;     /* VARIABLE op for this atom */
        uint32_t join_op_idx;    /* JOIN op that introduces this atom (0 for first) */
    } *atoms;

    uint32_t *join_op_indices;
    uint32_t var_op_index;
} col_multipass_rule_t;
```

---

## 3. CSE Strategy: What to Materialize

### 3.1 Materialization Policy

**Materialize prefix joins for atoms 0..K-2** of each K-atom recursive rule (K >= 3).

Rationale:
- The prefix `A1_full x A2_full x ... x A_{K-1}_full` is identical across all K delta passes.
- Only passes where delta position i < K-1 can reuse the prefix (passes 0..K-2).
- The last pass (delta position K-1) needs the full prefix, which is the most expensive
  and benefits most from caching.

### 3.2 Memory Limits

```c
#define COL_MAT_CACHE_DEFAULT_LIMIT (256 * 1024 * 1024)  /* 256 MB */
```

**Eviction policy:** LRU by last-access iteration. When `mem_used > mem_limit`:
1. Evict the entry with the oldest last-access iteration.
2. If all entries are from the current iteration, evict the entry with the
   fewest reuses (lowest benefit).
3. Never evict an entry that is currently being used by an active delta pass.

### 3.3 Staleness Detection

A materialized prefix `M[1..j]` is **stale** if any atom A1..Aj has a non-empty delta
in the current iteration. Stale entries must be recomputed.

```c
static bool
mat_entry_is_stale(const col_materialized_join_t *entry,
                   const wl_plan_stratum_t *sp,
                   wl_col_session_t *sess)
{
    for (uint32_t i = 0; i < entry->atom_count && i <= entry->prefix_depth; i++) {
        col_rel_t *r = session_find_rel(sess, sp->relations[entry->rule_idx].name);
        if (r && r->nrows != entry->atom_versions[i])
            return true;
    }
    return false;
}
```

In practice, since every iteration of a recursive stratum may produce new deltas for
any IDB relation, **most materialized entries will be stale each iteration**. The primary
benefit is reuse *within* a single iteration across the K delta passes for the same rule.

---

## 4. Progressive Refinement Example: DOOP CallGraphEdge 8-Way Join

### 4.1 The Rule

```datalog
CallGraphEdge(invocation, toMethod) :-
    Reachable(inMethod),                          -- A1
    Instruction_Method(invocation, inMethod),      -- A2
    VirtualMethodInvocation_Base(invocation, base), -- A3
    VarPointsTo(heap, base),                       -- A4
    HeapAllocation_Type(heap, heaptype),            -- A5
    VirtualMethodInvocation_SimpleName(invocation, simplename), -- A6
    VirtualMethodInvocation_Descriptor(invocation, descriptor), -- A7
    MethodLookup(simplename, descriptor, heaptype, toMethod).  -- A8
```

### 4.2 Original Option 2 (Reverted)

Creates 8 plan copies, each with a different delta position:

```
Copy 1: delta(Reachable) x IM x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML
Copy 2: Reachable x delta(IM) x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML
...
Copy 8: Reachable x IM x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x delta(ML)
```

**Cost:** 8 copies x 7 joins = 56 join operations per iteration.

### 4.3 CSE Redesign

**Phase 1: Materialize prefix (once per iteration)**

```
M[1]     = Reachable_full
M[1..2]  = Reachable_full x IM_full
M[1..3]  = M[1..2] x VMI_Base_full
M[1..4]  = M[1..3] x VPT_full
M[1..5]  = M[1..4] x HAT_full
M[1..6]  = M[1..5] x VMI_SN_full
M[1..7]  = M[1..6] x VMI_Desc_full
```

**Cost:** 7 joins (computed once, not 8 times).

**Phase 2: Delta passes (using materialized prefixes)**

```
Pass 1: delta(Reachable) x IM x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML
         = delta(Reachable) then 7 joins with full relations
Pass 2: M[1..1] x delta(IM) x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML
         = Reachable_full x delta(IM) then 6 joins
Pass 3: M[1..2] x delta(VMI_Base) x VPT x HAT x VMI_SN x VMI_Desc x ML
         = (Reachable x IM)_cached x delta(VMI_Base) then 5 joins
...
Pass 8: M[1..7] x delta(ML)
         = (Reachable x ... x VMI_Desc)_cached x delta(ML) = 1 join!
```

**Cost:** 7 (prefix) + 7+6+5+4+3+2+1+0 = 7+28 = 35 join operations.
**vs. Original:** 56 joins. **Savings: 37%.**

**With suffix materialization (optional):**

```
S[2..8] = IM x VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML  (7 joins)
S[3..8] = VMI_Base x VPT x HAT x VMI_SN x VMI_Desc x ML       (6 joins)
...
```

Each pass becomes: `prefix[i-1] x delta(Ai) x suffix[i+1]` = 2 joins.
**Cost:** 7 (prefix) + 7 (suffix) + 8*2 = 30 joins, but double memory.

### 4.4 Cost Estimate: Original vs CSE

| Approach | Plan Copies | Joins/Iteration | Memory Overhead |
|----------|-------------|-----------------|-----------------|
| Original Option 2 | 8 | 56 | None (plan only) |
| CSE (prefix only) | 1 | 35 | 1 materialized chain |
| CSE (prefix+suffix) | 1 | 30 | 2 materialized chains |

**For CSPA 3-way joins:**

| Approach | Plan Copies | Joins/Iteration | Overhead |
|----------|-------------|-----------------|----------|
| Original Option 2 | 3 | 6 | None |
| CSE (prefix only) | 1 | 2+3 = 5 | 1 cached join |

---

## 5. Cost Model: When Materialization Wins vs Recomputation

### 5.1 Decision Function

```
benefit(rule) = (K - 1) * cost(full_prefix_join) - cost(materialize_prefix)
                - sum(cost(suffix_joins_per_pass))
```

Where:
- `K` = number of body atoms
- `cost(join)` ~ `|left| * |right| / selectivity`
- `cost(materialize_prefix)` = cost of computing prefix once + memory for storage

### 5.2 Heuristic Thresholds

```c
/* Materialize when K >= 3 AND the prefix join produces fewer rows than
 * the product of individual relation sizes (i.e., joins are selective). */
static bool
should_materialize(uint32_t atom_count, uint32_t prefix_result_rows,
                   uint32_t product_of_full_sizes, size_t mem_available)
{
    if (atom_count < 3)
        return false;

    /* Memory check: materialized result must fit in budget */
    size_t row_bytes = prefix_result_rows * sizeof(int64_t) * MAX_COLS;
    if (row_bytes > mem_available)
        return false;

    /* Selectivity check: only materialize if joins are filtering */
    /* If the prefix produces more rows than the largest input, skip */
    return prefix_result_rows < product_of_full_sizes;
}
```

### 5.3 Break-Even Analysis

For a K-atom rule with uniform relation size N and join selectivity s:

- **Without CSE:** K plan copies, each doing K-1 joins = K*(K-1) joins
- **With CSE (prefix):** K-1 prefix joins + sum(K-1-i for i in 0..K-1) = K-1 + K*(K-1)/2

The crossover where CSE wins:
```
K*(K-1) > K-1 + K*(K-1)/2
K*(K-1)/2 > K-1
K/2 > 1
K > 2
```

**CSE always wins for K >= 3.** The benefit grows quadratically with K.

---

## 6. Edge Cases

### 6.1 Empty Delta

When `delta(Ai)` is empty for atom i, **skip that pass entirely**. The delta variant
produces zero tuples, so no joins need to be performed. This is already handled by
`col_op_variable` returning an empty relation when `WL_DELTA_FORCE_DELTA` finds no delta
(`exec_plan.h:602-611` / `columnar_nanoarrow.c:602-611`).

### 6.2 All-Duplicates Delta

When a delta contains only rows already present in the full relation, the CONSOLIDATE
step at the end of the iteration will eliminate duplicates. No special handling needed.
The iteration count is not affected because the delta is still non-empty (driving the
next iteration), but the new facts after consolidation will be zero, terminating the
fixed-point loop.

### 6.3 Single-Row Relations

For EDB relations with very few rows (e.g., `MainClass` with 1 row), materialization
overhead exceeds benefit. The cost model handles this: `prefix_result_rows` will be
tiny, and the join is already cheap.

### 6.4 Self-Joins

Rules where the same relation appears multiple times in the body (e.g.,
`valueFlow(x,y) :- valueFlow(x,z), valueFlow(z,y)`) require that each occurrence
independently selects delta or full. The multi-pass approach handles this correctly:
pass 0 uses `delta(vF) x vF_full`, pass 1 uses `vF_full x delta(vF)`.

### 6.5 Mixed EDB/IDB Atoms

Some atoms in a rule body may be EDB (never change). Delta for EDB atoms is always
empty, so those passes are skipped automatically. This is correct: EDB atoms don't
produce new facts, so `delta(EDB) x anything` = empty.

### 6.6 Negation Interactions

Rules with ANTIJOIN (negation) atoms cannot have delta applied to the negated atom
(semi-naive doesn't expand delta for negated atoms). The multi-pass loop should
skip delta positions corresponding to negated atoms. Negated atoms always use full.

---

## 7. Operator Pseudocode

### 7.1 col_op_materialize

```c
/**
 * Materialize a prefix join result into the cache.
 *
 * @cache:        The materialization cache.
 * @rule_idx:     Index of the rule in the stratum.
 * @prefix_depth: Number of atoms joined (0-based: depth=2 means A1 x A2 x A3).
 * @result:       The join result to cache (ownership transferred to cache).
 * @atom_nrows:   Current nrows for each atom (for staleness detection).
 * @atom_count:   Total number of atoms in the rule.
 *
 * Returns 0 on success, ENOMEM if eviction fails to free enough space.
 */
static int
col_op_materialize(col_mat_cache_t *cache, uint32_t rule_idx,
                   uint32_t prefix_depth, col_rel_t *result,
                   const uint32_t *atom_nrows, uint32_t atom_count)
{
    size_t entry_bytes = (size_t)result->nrows * result->ncols * sizeof(int64_t);

    /* Evict if necessary */
    while (cache->mem_used + entry_bytes > cache->mem_limit
           && cache->count > 0) {
        /* LRU eviction: remove oldest entry */
        col_materialized_join_t *victim = &cache->entries[0];
        size_t victim_bytes
            = (size_t)victim->result->nrows * victim->ncols * sizeof(int64_t);
        col_rel_free_contents(victim->result);
        free(victim->result);
        free(victim->atom_versions);
        cache->mem_used -= victim_bytes;
        /* Shift remaining entries */
        memmove(&cache->entries[0], &cache->entries[1],
                (cache->count - 1) * sizeof(col_materialized_join_t));
        cache->count--;
    }

    /* Check if we still can't fit */
    if (cache->mem_used + entry_bytes > cache->mem_limit)
        return ENOMEM;  /* skip materialization, fall back to recomputation */

    /* Grow entries array if needed */
    if (cache->count >= cache->capacity) {
        uint32_t new_cap = cache->capacity ? cache->capacity * 2 : 4;
        col_materialized_join_t *new_entries = (col_materialized_join_t *)realloc(
            cache->entries, new_cap * sizeof(col_materialized_join_t));
        if (!new_entries)
            return ENOMEM;
        cache->entries = new_entries;
        cache->capacity = new_cap;
    }

    /* Store entry */
    col_materialized_join_t *entry = &cache->entries[cache->count++];
    entry->rule_idx = rule_idx;
    entry->prefix_depth = prefix_depth;
    entry->result = result;
    entry->atom_count = atom_count;
    entry->ncols = result->ncols;
    entry->atom_versions = (uint32_t *)malloc(atom_count * sizeof(uint32_t));
    if (!entry->atom_versions) {
        cache->count--;
        return ENOMEM;
    }
    memcpy(entry->atom_versions, atom_nrows, atom_count * sizeof(uint32_t));
    cache->mem_used += entry_bytes;

    return 0;
}
```

### 7.2 col_op_lookup_materialized

```c
/**
 * Look up a materialized prefix join in the cache.
 *
 * @cache:        The materialization cache.
 * @rule_idx:     Index of the rule.
 * @prefix_depth: Desired prefix depth.
 * @atom_nrows:   Current atom nrows (for staleness check).
 *
 * Returns the cached relation (borrowed, do NOT free), or NULL if
 * not found or stale.
 */
static col_rel_t *
col_op_lookup_materialized(const col_mat_cache_t *cache, uint32_t rule_idx,
                           uint32_t prefix_depth, const uint32_t *atom_nrows)
{
    for (uint32_t i = 0; i < cache->count; i++) {
        const col_materialized_join_t *e = &cache->entries[i];
        if (e->rule_idx != rule_idx || e->prefix_depth != prefix_depth)
            continue;

        /* Staleness check: all participating atoms must have same nrows */
        bool stale = false;
        for (uint32_t a = 0; a <= prefix_depth && a < e->atom_count; a++) {
            if (e->atom_versions[a] != atom_nrows[a]) {
                stale = true;
                break;
            }
        }
        if (!stale)
            return e->result;
    }
    return NULL;
}
```

### 7.3 col_op_union_delta_variants

```c
/**
 * Evaluate a K-atom rule with CSE multi-pass delta expansion.
 * Computes union of all K delta variants using materialized prefix joins.
 *
 * @rplan:    The relation plan (containing the rule's ops).
 * @sp:       The stratum plan (for relation name lookups).
 * @sess:     The session (for relation data access).
 * @cache:    The materialization cache.
 * @rule_idx: Index of this rule in the stratum.
 *
 * Returns the unioned result (owned by caller) on success, NULL on error.
 */
static col_rel_t *
col_op_union_delta_variants(const wl_plan_relation_t *rplan,
                            const wl_plan_stratum_t *sp,
                            wl_col_session_t *sess,
                            col_mat_cache_t *cache,
                            uint32_t rule_idx)
{
    /* Step 1: Identify VARIABLE and JOIN ops to determine atom count.
     * Walk the ops array: first VARIABLE is atom 0, each subsequent
     * JOIN introduces atom N+1. */
    uint32_t atom_count = 0;
    uint32_t var_indices[MAX_ATOMS];   /* op index of VARIABLE for each atom */
    uint32_t join_indices[MAX_ATOMS];  /* op index of JOIN for atom i (i>0) */

    for (uint32_t i = 0; i < rplan->op_count; i++) {
        if (rplan->ops[i].op == WL_PLAN_OP_VARIABLE) {
            var_indices[atom_count] = i;
            atom_count++;
        } else if (rplan->ops[i].op == WL_PLAN_OP_JOIN) {
            join_indices[atom_count - 1] = i;  /* JOIN follows previous VARIABLE */
        }
    }

    if (atom_count < 3) {
        /* 2-atom rules: use existing evaluation (already correct) */
        return NULL;  /* signal: use default path */
    }

    /* Step 2: Collect current atom nrows for staleness detection */
    uint32_t atom_nrows[MAX_ATOMS];
    for (uint32_t a = 0; a < atom_count; a++) {
        const char *rname = rplan->ops[var_indices[a]].relation_name;
        col_rel_t *rel = session_find_rel(sess, rname);
        atom_nrows[a] = rel ? rel->nrows : 0;
    }

    /* Step 3: Compute or retrieve materialized prefix joins */
    col_rel_t *prefixes[MAX_ATOMS];
    memset(prefixes, 0, sizeof(prefixes));

    /* prefix[0] = full(atom[0]) */
    col_rel_t *a0_full = session_find_rel(sess,
        rplan->ops[var_indices[0]].relation_name);
    prefixes[0] = a0_full;  /* borrowed */

    for (uint32_t depth = 1; depth < atom_count - 1; depth++) {
        /* Try cache lookup first */
        col_rel_t *cached = col_op_lookup_materialized(
            cache, rule_idx, depth, atom_nrows);
        if (cached) {
            prefixes[depth] = cached;  /* borrowed from cache */
            continue;
        }

        /* Compute: prefixes[depth] = prefixes[depth-1] x full(atom[depth]) */
        col_rel_t *right = session_find_rel(sess,
            rplan->ops[var_indices[depth]].relation_name);
        if (!right)
            continue;

        /* Perform join using the JOIN op's key specification */
        col_rel_t *joined = perform_hash_join(
            prefixes[depth - 1], right,
            &rplan->ops[join_indices[depth]], sess);
        if (!joined)
            continue;

        /* Store in cache (ownership transfers) */
        col_rel_t *to_cache = col_rel_clone(joined);  /* clone for cache */
        if (to_cache) {
            col_op_materialize(cache, rule_idx, depth,
                               to_cache, atom_nrows, atom_count);
        }
        prefixes[depth] = joined;
    }

    /* Step 4: For each delta position, compute the delta variant */
    col_rel_t *result = col_rel_new_auto("$delta_union", 0);
    if (!result)
        return NULL;

    for (uint32_t delta_pos = 0; delta_pos < atom_count; delta_pos++) {
        /* Check if delta exists for this atom */
        const char *rname = rplan->ops[var_indices[delta_pos]].relation_name;
        char dname[256];
        snprintf(dname, sizeof(dname), "$d$%s", rname);
        col_rel_t *delta = session_find_rel(sess, dname);
        if (!delta || delta->nrows == 0)
            continue;  /* skip: no new facts for this atom */

        /* Build variant:
         * Left side:   prefix[delta_pos - 1] (or nothing if delta_pos == 0)
         * Delta atom:  delta(atom[delta_pos])
         * Right side:  full(atom[delta_pos+1]) x ... x full(atom[K-1]) */

        col_rel_t *variant;
        if (delta_pos == 0) {
            variant = col_rel_clone(delta);
        } else {
            variant = perform_hash_join(
                prefixes[delta_pos - 1], delta,
                &rplan->ops[join_indices[delta_pos]], sess);
        }
        if (!variant)
            continue;

        /* Join remaining atoms (delta_pos+1 .. K-1) using full relations */
        for (uint32_t j = delta_pos + 1; j < atom_count; j++) {
            col_rel_t *right = session_find_rel(sess,
                rplan->ops[var_indices[j]].relation_name);
            if (!right) {
                col_rel_free_contents(variant);
                free(variant);
                variant = NULL;
                break;
            }
            col_rel_t *next = perform_hash_join(
                variant, right, &rplan->ops[join_indices[j]], sess);
            col_rel_free_contents(variant);
            free(variant);
            variant = next;
            if (!variant)
                break;
        }

        if (variant) {
            /* Apply remaining ops (MAP, FILTER, etc.) after all joins */
            /* ... apply_post_join_ops(variant, rplan, atom_count) ... */

            col_rel_append_all(result, variant);
            col_rel_free_contents(variant);
            free(variant);
        }
    }

    return result;
}
```

---

## 8. Rule Analysis

### 8.1 CSPA: Two 3-Way Recursive Rules

**Rule 1: memoryAlias** (`cspa.dl:15`)

```datalog
memoryAlias(x, w) :- dereference(y, x), valueAlias(y, z), dereference(z, w).
```

- Atoms: `dereference` (EDB), `valueAlias` (IDB), `dereference` (EDB, self-join)
- K = 3
- Delta passes needed: 3 (but EDB deltas are always empty after initial load)
- **Effective passes: 1** (only `delta(valueAlias)` produces new facts)
- CSE benefit: Minimal for K=3, but prefix `deref x valAlias` is reusable

**Rule 2: valueAlias** (`cspa.dl:17`)

```datalog
valueAlias(x, y) :- valueFlow(z, x), memoryAlias(z, w), valueFlow(w, y).
```

- Atoms: `valueFlow` (IDB), `memoryAlias` (IDB), `valueFlow` (IDB, self-join)
- K = 3
- All 3 atoms are IDB (mutually recursive), all 3 passes are needed
- **CSE benefit: Moderate.** Prefix `vFlow x memAlias` computed once, reused for pass 2.
- Original Option 2: 3 copies x 2 joins = 6 joins
- CSE: 2 (prefix) + 2+1+0 = 5 joins (17% reduction)

### 8.2 DOOP: Three 8-Way Recursive Rules

**Rule 1: CallGraphEdge (virtual dispatch)** (`doop.dl:412`)

```datalog
CallGraphEdge(invocation, toMethod) :-
    Reachable(inMethod), Instruction_Method(invocation, inMethod),
    VirtualMethodInvocation_Base(invocation, base), VarPointsTo(heap, base),
    HeapAllocation_Type(heap, heaptype),
    VirtualMethodInvocation_SimpleName(invocation, simplename),
    VirtualMethodInvocation_Descriptor(invocation, descriptor),
    MethodLookup(simplename, descriptor, heaptype, toMethod).
```

- K = 8, all in Phase 4 recursive stratum
- IDB atoms: `Reachable`, `VarPointsTo`, `MethodLookup` (3 IDB, 5 EDB)
- **Effective passes: 3** (only IDB atoms produce deltas)
- Original Option 2: 8 copies x 7 joins = 56 total joins
- CSE: 7 (prefix) + at most 3 active passes ~ **10-12 joins** (78-82% reduction!)

**Rule 2: Reachable (virtual dispatch)** (`doop.dl:411`) -- identical body to CallGraphEdge

**Rule 3: VarPointsTo (virtual dispatch)** (`doop.dl:402`) -- 9-way join:

```datalog
VarPointsTo(heap, this) :-
    Reachable(inMethod), Instruction_Method(invocation, inMethod),
    VirtualMethodInvocation_Base(invocation, base), VarPointsTo(heap, base),
    HeapAllocation_Type(heap, heaptype),
    VirtualMethodInvocation_SimpleName(invocation, simplename),
    VirtualMethodInvocation_Descriptor(invocation, descriptor),
    MethodLookup(simplename, descriptor, heaptype, toMethod),
    ThisVar(toMethod, this).
```

- K = 9
- Original Option 2: 9 copies x 8 joins = 72 joins
- CSE: 8 (prefix) + ~3 active passes ~ **11-14 joins** (80-85% reduction!)

### 8.3 Cost Estimate Summary

| Workload | Rule | K | Original Copies | CSE Passes | Join Reduction |
|----------|------|---|-----------------|------------|----------------|
| CSPA | memoryAlias | 3 | 3 | ~1 effective | 50% |
| CSPA | valueAlias | 3 | 3 | 3 | 17% |
| DOOP | CallGraphEdge | 8 | 8 | ~3 effective | 78% |
| DOOP | Reachable (virt) | 8 | 8 | ~3 effective | 78% |
| DOOP | VarPointsTo (virt) | 9 | 9 | ~3 effective | 80% |

---

## 9. Implementation Integration Points

### 9.1 Files to Modify

| File | Change | Effort |
|------|--------|--------|
| `wirelog/backend/columnar_nanoarrow.c` | Add CSE evaluation loop in `col_eval_stratum` | High |
| `wirelog/backend/columnar_nanoarrow.c` | Add `col_mat_cache_t` and helper functions | Medium |
| `wirelog/exec_plan.h` | No changes needed (delta_mode already present) | None |
| `wirelog/exec_plan_gen.c` | No plan rewriting needed (runtime approach) | None |

### 9.2 Integration into col_eval_stratum

The key change is in the recursive stratum loop (`columnar_nanoarrow.c:1695-1780`):

```c
/* Current: single-pass evaluation */
for (uint32_t ri = 0; ri < nrels; ri++) {
    col_eval_relation_plan(rp, &stack, sess);
    /* ... append to target ... */
}

/* Proposed: multi-pass for K>=3 rules */
for (uint32_t ri = 0; ri < nrels; ri++) {
    const wl_plan_relation_t *rp = &sp->relations[ri];

    if (rule_has_k_atoms(rp) >= 3 && sp->is_recursive) {
        col_rel_t *result = col_op_union_delta_variants(
            rp, sp, sess, &mat_cache, ri);
        if (result) {
            col_rel_append_all(target, result);
            col_rel_free_contents(result);
            free(result);
            continue;
        }
        /* Fallback to original evaluation if CSE fails */
    }

    /* Original single-pass evaluation */
    eval_stack_t stack;
    eval_stack_init(&stack);
    col_eval_relation_plan(rp, &stack, sess);
    /* ... */
}
```

### 9.3 Key Design Decision: Runtime vs Compile-Time

The original Option 2 used **compile-time plan rewriting** (emitting K plan copies).
This redesign uses **runtime iteration** within the evaluator. Advantages:

- No plan size explosion (the plan remains unchanged)
- Materialization decisions can adapt per-iteration based on actual data sizes
- Easier to implement incrementally (add CSE later without changing the plan format)
- Reverts cleanly (just remove the runtime loop; plan is untouched)

Trade-off: Slightly more complex evaluator logic, but avoids the plan generator changes
that were the source of the original regression.

---

## 10. References

- `wirelog/exec_plan.h:130-157` -- `wl_delta_mode_t` enum definition
- `wirelog/exec_plan.h:215-238` -- `wl_plan_op_t` struct with `delta_mode` field
- `wirelog/backend/columnar_nanoarrow.c:578-620` -- `col_op_variable` with delta_mode handling
- `wirelog/backend/columnar_nanoarrow.c:758-811` -- `col_op_join` with delta_mode handling
- `wirelog/backend/columnar_nanoarrow.c:1547-1870` -- `col_eval_stratum` fixed-point loop
- `wirelog/exec_plan_gen.c:532-794` -- `translate_ir_node` (plan generation, NOT modified)
- `bench/workloads/cspa.dl:15-17` -- CSPA 3-way join rules
- `bench/workloads/doop.dl:402-412` -- DOOP 8/9-way join rules
- `docs/performance/PHASE-2C-COMPLETION-REPORT.md:36-41` -- Option 2 revert rationale
- `docs/performance/OPTIMIZATION-STRATEGY.md:189-334` -- Original Option 2 specification
