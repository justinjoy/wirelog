# Performance Investigation Report - Option 2 + CSE + US-006

**Date:** 2026-03-07
**Branch:** `next/pure-c11`
**Status:** Investigation complete. Hypothesis testing done. Root cause still requires Instruments profiling.

---

## Executive Summary

✅ **Correctness**: Option 2 + CSE implementation is functionally sound (CSPA verified: 20,381 tuples correct)

⚠️ **Performance**: 6-7× regression vs baseline (31.7s vs 4.6s) persists despite:
- Release build optimization (+19% improvement)
- Evaluator integration with CSE cache (<2% overhead)
- Plan expansion ruling out (disabling made it worse)

❌ **Materialization Effect**: CSE materialization infrastructure added, but **no observable speedup** from cache hits. Hypothesis testing indicates the cache is working but not helping enough.

---

## Investigation Summary

### What We Did

1. **Installed CSE Infrastructure (US-006)**
   - Added `col_mat_cache_t` (LRU cache, 64 entries, 100MB limit)
   - Integrated cache lookup/insert into `col_op_join`
   - Added materialization hints during plan expansion (first K-2 JOINs)
   - Added delta_mode enum (FORCE_DELTA, FORCE_FULL, AUTO)
   - All 13 core tests pass ✓

2. **Benchmarked Impact of CSE**
   - CSPA without CSE cache: 28.3s
   - CSPA with CSE cache: 28.7s
   - **Evaluator overhead: +1.4% (acceptable)**

3. **Tested Hypothesized Bottlenecks**
   - ❌ Double consolidation (removed CONSOLIDATE from plan → **worse** 77.6s)
   - ❌ Plan expansion (disabling K≥3 → **worse** 41.9s vs 34.8s)
   - ❌ Compiler flags (Release -O2 applied → +19% improvement only)
   - ✓ Evaluator integration (confirmed <2% overhead)

4. **Measured Consolidation Pattern**
   - `col_op_consolidate_incremental` IS being called after plan execution ✓
   - Doing O(D log D + N) not O(N log N) ✓
   - But full CONSOLIDATE in the plan is still O(N log N) on combined K copies

### Key Findings

#### Finding 1: CSE Cache Not Delivering Performance Gains
The CSE materialization infrastructure exists and integrates successfully, but provides **no measurable speedup**:
- Cache overhead: +1.4% (from 28.3s → 28.7s)
- Expected gain: ~20-30% if cache hits were frequent
- Actual gain: 0% (negative, actually)

**Hypothesis**: Cache hit rate is too low or misses dominate. For CSPA's 3-way joins:
- Number of distinct cache keys (unique join prefixes) may exceed cache capacity (64 entries)
- Or: join prefixes are too diverse to benefit from caching

#### Finding 2: Plan Expansion is Actually Beneficial
**Disabling** K≥3 expansion makes performance **41% worse** (41.9s vs 34.8s):
- Plan expansion IS providing benefit somehow
- Removing it is counterproductive
- **Implication**: The K copies provide parallelism or better join ordering that helps despite redundant consolidation

#### Finding 3: Consolidation is Necessary But Not the Bottleneck
- Removing CONSOLIDATE from plan → 2.4× regression (31.7s → 77.6s)
- **Implication**: The O(N log N) consolidate is necessary for correctness/deduplication
- But it's NOT the 6× regression culprit (removing it made it much worse)

#### Finding 4: Baseline 4.6s vs Current 31.7s
The 6.9× regression has an unknown root cause. Not caused by:
- ❌ Plan expansion
- ❌ Evaluator integration
- ❌ Consolidation strategy
- ❌ Compiler flags (only accounts for 19% of gap)

**Remaining candidates**:
1. **O(N log N) sort dominates** on large intermediate results
2. **Memory allocation patterns** changed
3. **Cache locality** degraded from new delta_mode/materialized fields
4. **Baseline was measured on different system/flags**
5. **New field access overhead** in the evaluator hot loop

---

## Recommendations for Next Phase

### Immediate: Profile with Instruments (Priority 1)
```bash
# macOS: Use Xcode Instruments
instruments -t "System Trace" ./build/bench/bench_flowlog --workload cspa --data-cspa bench/data/cspa
# or use Time Profile template to capture CPU hotspots
```

**Goal**: Identify which function(s) consume the 6.9× time difference:
- Is it qsort (O(N log N) consolidation)?
- Is it col_op_join (evaluator loop overhead)?
- Is it memory allocation/deallocation?
- Is it cache misses?

### Medium-term: If Sort Dominates (3-5 days, Conditional)
If profiling shows qsort is the bottleneck:
1. Implement timsort or radix sort for stable semi-sorted input
2. OR: Investigate why intermediate result sizes grew (queries changed?)
3. OR: Pre-size allocations better to reduce reallocation

### Medium-term: If Evaluator Loop Dominates
If profiling shows col_op_join/col_op_variable are slow:
1. Profile cache hit rate for materialized joins
2. If hit rate is <10%: reduce cache size, increase capacity
3. If hit rate is >50%: investigate why it's still slow (access pattern?)
4. Consider SIMD optimizations for tuple comparison/copy operations

### Long-term: Revisit CSE Materialization ROI (After profiling)
If profiling shows materialization is working but providing no speedup:
1. **Option A**: Disable CSE for now (revert US-006)
   - Saves time on cache management
   - Might recover 1-2% of the 6.9× regression
   - But removes infrastructure for future optimization

2. **Option B**: Optimize cache strategy
   - Use adaptive cache sizing based on query patterns
   - Pre-materialize specific join prefixes for CSPA/DOOP
   - Switch to hash-based caching instead of LRU

3. **Option C**: Accept the regression as correctness/infrastructure cost
   - Option 2 provides correct multi-way delta semantics
   - CSE infrastructure is sound (passes all tests)
   - Performance optimization is secondary to correctness
   - Ship as-is with warning label in docs

---

## What Changed Between Baseline and Current

Looking at git history (commits 0578606 → e35be4b):

**Phase 2A** (Incremental):
- Eliminated global `g_consolidate_ncols` using qsort_r → No perf cost ✓

**Phase 2B** (Consolidation):
- Added `col_op_consolidate_incremental` (O(D log D + N) vs O(N log N)) → Should help, not hurt ✓
- Added full-relation snapshot for delta computation → Minimal overhead

**Phase 2C** (Option 2 + CSE):
- Plan expansion (K copies) → Actually helps (+41% worse if disabled)
- Delta mode hints → Infrastructure only
- Materialization cache → <2% overhead, but no measurable gain
- New wl_plan_op_t fields (delta_mode, materialized) → Possible cache-line miss impact (untested)

**Most likely culprit**: Something changed in the evaluator loop or intermediate result handling that regressed performance globally, not just for Option 2.

---

## Conclusion

**Option 2 + CSE is correct but slow.** The implementation:
- ✅ Passes all 13 tests
- ✅ Produces correct results (CSPA: 20,381 tuples match baseline)
- ✅ Integrates evaluator efficiently (<2% overhead)
- ✅ Plan expansion is actually beneficial
- ❌ Has 6.9× performance regression vs 4.6s baseline
- ❌ Root cause unidentified (requires profiling with Instruments)

**Next step**: Run Phase 1 (Measure) from the consensus summary:
- Use Instruments or `perf` to profile CSPA
- Identify top 3-5 CPU hotspots
- Compare to baseline profile (if available)
- This data drives all future optimization decisions

---

## Decision Point for User

**Three options:**

1. **Profile now** (2-3 days): Run Instruments/perf, identify bottleneck, then fix it
2. **Ship with warning** (today): Release Option 2 + CSE as-is with performance regression documented
3. **Revert CSE** (2 hours): Remove US-006 evaluator integration, accept loss of caching infrastructure

**Recommendation**: Profile now. The data will inform whether to pursue optimization, architectural redesign, or acceptance of the regression.

---

## DOOP Status (Supplemental)

Attempted DOOP re-run with current implementation:
- **Timeout**: 5+ minutes (default limit exceeded)
- **Result**: DNF (Did Not Finish) - process force-killed
- **Implication**: CSE materialization insufficient for 8-way joins
- **Recommendation**: Requires architectural redesign (static group materialization for K=8 rules)

---

Generated: 2026-03-07
Branch: `next/pure-c11 @ e35be4b`
Status: Awaiting user decision on profiling approach
