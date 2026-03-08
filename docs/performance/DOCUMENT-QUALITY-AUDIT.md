# Document Quality Audit

**Date:** 2026-03-08
**Auditor:** Quality Reviewer (Specialist)
**Scope:** 7 primary performance strategy documents + 3 supplementary documents

---

## Summary

**Overall Quality: NEEDS WORK**

The document set demonstrates thorough analysis and a sound strategic conclusion (K-fusion evaluator rewrite), but suffers from significant internal contradictions between pre- and post-architect-review documents, one confirmed code-level correctness bug in the implemented merge function, and unsupported or circular performance claims. Several documents were not updated after the architect review, leaving stale numbers and team sizing that contradict the approved strategy.

---

## Individual Document Reviews

### 1. BOTTLENECK-PROFILING-ANALYSIS.md

- **Completeness:** WARNING
- **Data Quality:** WARNING

**Issues:**

1. **[HIGH] Unsupported profiling data (lines 37-44).** The "Top Functions by Wall Time" table lists precise percentages (29%, 12%, 8%, 18%, 12%) but cites no profiling tool output or methodology. These read as estimates, not measurements. A real `perf report` or Instruments trace should be referenced.

2. **[MEDIUM] Contradictory improvement claims.** Line 168 claims "50-60% wall-time reduction" for Path D (Evaluator Rewrite), but lines 259-298 of IMPLEMENTATION-PATHS-ANALYSIS.md show the architect working through the math in real-time and arriving at only 2.1%, then being corrected to 30-40%. The BOTTLENECK doc was not updated to reflect this correction.

3. **[MEDIUM] "12 full clones" claim (line 66).** The ARCHITECT-VERIFICATION-FINAL.md (line 40) explicitly notes that `VARIABLE` operator uses borrowed references, not clones. The "2GB additional peak RSS from clones" claim needs profiling evidence; it may be overstated.

4. **[LOW] Path D labeled "BREAKTHROUGH" in header (line 142).** This pre-loads bias. The document should present options neutrally and let analysis speak.

### 2. IMPLEMENTATION-PATHS-ANALYSIS.md

- **Completeness:** PASS (with caveats)
- **Rigor:** WARNING

**Issues:**

1. **[HIGH] Self-contradicting math preserved in final document (lines 259-298).** The document contains an extended inline "working through the math" section where the author calculates 2.1% improvement, then receives an architect correction. This working-out was left in the final document. It undermines confidence: the reader cannot tell which numbers to trust. The final answer (30-40%) appears correct per architect, but the derivation is unconvincing because it relies on a vague "all factors" argument rather than rigorous breakdown.

2. **[MEDIUM] Path labeling inconsistency.** In BOTTLENECK-PROFILING-ANALYSIS.md, paths are labeled A (K-Way Merge), B (Incremental Consolidate), C (Empty-Delta Skip), D (Evaluator Rewrite). In IMPLEMENTATION-PATHS-ANALYSIS.md, paths are relabeled: A (Incremental Consolidate), B (Evaluator Rewrite), C (Hybrid). This creates confusion when cross-referencing documents.

3. **[MEDIUM] Missing exploration of alternative paths.** The analysis considers only incremental consolidate, evaluator rewrite, and hybrid. Not explored:
   - **Columnar sort optimization** (radix sort instead of qsort for int64 data -- O(n) vs O(n log n))
   - **Lazy consolidation** (defer consolidate until results are actually consumed by a join)
   - **Rule-level memoization** (cache join results across iterations when inputs unchanged)
   - **Stratum-level parallelism** (parallelize independent strata rather than K-copies within a stratum)

4. **[LOW] Effort estimate of 27 hours for Path B (line 314) conflicts with roadmap's 100-150 hours (BREAKTHROUGH-STRATEGY-ROADMAP.md line 273).** The two documents define "scope" differently, but this is confusing.

### 3. BREAKTHROUGH-STRATEGY-ROADMAP.md

- **Completeness:** PASS
- **Clarity:** WARNING
- **Feasibility:** PASS

**Issues:**

1. **[HIGH] Internal contradiction in team sizing.** The roadmap body says "1 Engineer, 2-3 Week Sprint" (line 237) as the primary recommendation. But the "Next Steps" section (line 402) says "Allocate 2 engineers for 4-week sprint." These are vestigial pre-architect text that was not cleaned up.

2. **[MEDIUM] Week 1 and Week 2 task duplication.** Day 2-3 of Week 1 (line 87-88) covers "Refactor col_eval_relation_plan() for K-fusion dispatch." Day 6 of Week 2 (line 123-126) covers the same task again: "Refactor semi-naive loop for K-fusion." This reads as though the same work was scheduled twice.

3. **[MEDIUM] Success criteria mismatch.** Line 161 says "CSPA wall-time < 16 seconds (50% improvement)" as a success criterion, but the architect-corrected target (same document, line 299) is "18-20 seconds (30-40% improvement)." These are both in the same document.

4. **[LOW] "Atomic Commits" list (lines 208-214) includes "feat: implement workqueue backend" but the roadmap itself says workqueue already exists. This commit message is misleading.

### 4. K-FUSION-DESIGN.md

- **Completeness:** PASS
- **Technical Accuracy:** WARNING

**Issues:**

1. **[CRITICAL] `col_rel_merge_k` signature mismatch with implementation.** The design document (line 226) specifies:
   ```c
   col_rel_t *col_rel_merge_k(col_rel_t **relations, uint32_t k, wl_arena_t *arena);
   ```
   The actual implementation (`columnar_nanoarrow.c:2138`) has:
   ```c
   static col_rel_t *col_rel_merge_k(col_rel_t **relations, uint32_t k);
   ```
   The arena parameter was dropped. Design doc not updated.

2. **[CRITICAL] `col_eval_k_fusion()` pseudo-code (lines 154-217) uses `malloc` for worker_arenas and `alloca` for results.** The design references `wl_arena_t` and `col_workqueue_t` APIs that do not match the actual `wl_workqueue_submit` signature. These pseudo-code details will mislead implementers.

3. **[MEDIUM] Design references `col_eval_relation_plan()` at "lines 2540-2730" (line 63).** Actual function location differs in current codebase (K-fusion code is at lines 2285-2313). Line references are fragile and already stale.

### 5. IMPLEMENTATION-TASK-BREAKDOWN.md

- **Completeness:** PASS
- **Realism:** WARNING
- **Test Adequacy:** WARNING

**Issues:**

1. **[HIGH] Test strategy does not cover the memcmp bug.** The existing `col_rel_merge_k` implementation uses `memcmp` for row comparison (lines 2170, 2190, 2207, 2218, 2229), but commit `aba8fc7` already fixed a bug where `memcmp` produces wrong ordering on little-endian systems for int64_t data. The test cases in `test_k_fusion_merge.c` only test small values (0, 1, 2, 3, ...) where memcmp and lexicographic comparison agree. **There is no test with values where byte order matters (e.g., 254 vs 256).** This is a latent correctness bug.

2. **[MEDIUM] Task 3.4 DOOP validation (line 207-222) estimates "5-10 minutes + analysis" but the document elsewhere says DOOP currently times out. If DOOP still times out after K-fusion, the task breakdown provides no debugging strategy beyond "investigate."

3. **[LOW] Critical path diagram (lines 266-274) shows Task 2.2, 2.3, and 2.4 as parallel after 2.1, but they all modify the same file (columnar_nanoarrow.c). Merge conflicts are inevitable.

### 6. ARCHITECT-VERIFICATION-FINAL.md

- **Completeness:** PASS
- **Rigor:** PASS
- **Sign-off Criteria:** PASS

**Issues:**

1. **[MEDIUM] Verification of clone overhead (line 40) says "This must be profiled to confirm actual memory overhead" but no follow-up action is tracked.** This remains an open assumption.

2. **[MEDIUM] Math inconsistency in CSPA section (lines 106-115).** The architect calculates 2-2.5% savings from parallelism alone, then jumps to "30-40% is realistic estimate" by appeal to indirect costs (memory pressure, cache locality). This 15x gap is not adequately bridged. The 30-40% claim remains weakly supported.

3. **[LOW] Conditions 1 and 2 are marked "COMPLETED" (lines 190, 200) but this document is the one making the corrections. Self-referential sign-off reduces confidence.

### 7. K-FUSION-ARCHITECTURE.md

- **Completeness:** PASS
- **Status Accuracy:** WARNING
- **Remaining Work Clarity:** PASS

**Issues:**

1. **[CRITICAL] Incorrect claim about comparison algorithm (line 72).** States "Uses lexicographic row comparison (memcmp)." These are contradictory: memcmp is NOT lexicographic for int64_t on little-endian systems. The actual code uses memcmp (bug), while `kway_row_cmp` and `row_cmp_fn` in the same file use proper lexicographic comparison. This was the exact bug fixed in commit `aba8fc7`.

2. **[MEDIUM] K>=3 merge claims "min-heap merge" (line 65) but actual implementation at line 2240-2244 uses pairwise sequential merge, not a min-heap.** The document is inaccurate about the algorithm used.

3. **[MEDIUM] Claims "18/19 tests passing (1 EXPECTEDFAIL)" (line 177) but does not identify which test is expected to fail or why. The should_fail test in meson.build (line 267) should be referenced.

---

## Cross-Document Consistency

### Contradictions Found

| Issue | Documents | Severity |
|-------|-----------|----------|
| **CSPA improvement target: 50-60% vs 30-40%** | EXECUTIVE-BREAKTHROUGH-SUMMARY (line 64: "50-60%"), ARCHITECT-VERIFICATION-FINAL (line 101: "30-40%"), BREAKTHROUGH-STRATEGY-ROADMAP (line 299: "30-40%") | HIGH |
| **Team size: 2 engineers/4 weeks vs 1 engineer/2-3 weeks** | EXECUTIVE-BREAKTHROUGH-SUMMARY (line 104: "2 Engineers, 4-Week Sprint"), BREAKTHROUGH-STRATEGY-ROADMAP line 237 ("1 Engineer, 2-3 Week Sprint"), same doc line 402 ("Allocate 2 engineers for 4-week sprint") | HIGH |
| **Effort estimate: 27 hours vs 100-150 hours vs 560 hours** | IMPLEMENTATION-PATHS-ANALYSIS (line 314: "27 hours"), BREAKTHROUGH-STRATEGY-ROADMAP (line 273: "100-150 hours"), EXECUTIVE-BREAKTHROUGH-SUMMARY (line 109: "560 hours") | HIGH |
| **Path labels (A/B/C) have different meanings** | BOTTLENECK-PROFILING-ANALYSIS vs IMPLEMENTATION-PATHS-ANALYSIS | MEDIUM |
| **Iteration count: "reduce to 1-2" vs "stays at 6"** | Initial text in IMPLEMENTATION-PATHS-ANALYSIS (line 327) vs ARCHITECT-VERIFICATION-FINAL (line 42) | HIGH (partially corrected) |
| **K>=3 algorithm: min-heap vs pairwise merge** | K-FUSION-ARCHITECTURE (line 65: "min-heap") vs actual code (line 2240: pairwise) | MEDIUM |
| **col_rel_merge_k signature: with arena vs without** | K-FUSION-DESIGN (line 226) vs actual implementation (line 2138) | MEDIUM |

### Gaps Between Documents

1. **No document tracks the memcmp bug across multiple functions.** The code has a known class of bug (memcmp on int64_t), was partially fixed in commit aba8fc7 (sort/dedup phases only), but memcmp remains in: (a) col_rel_merge_k (K-fusion merge, 6 call sites), (b) col_op_consolidate_incremental_delta Phase 2 merge (line 1822, in the very function aba8fc7 was supposed to fix), and (c) col_row_in_sorted binary search (line 3183, sort/search algorithm mismatch). No document flags any of these.

2. **No document provides actual `perf` output.** All profiling claims reference percentages without tool output. The architect explicitly requested profiling evidence by day 3, but there is no baseline measurement methodology documented.

3. **EXECUTIVE-BREAKTHROUGH-SUMMARY.md was never updated with architect corrections.** It still claims 50-60% CSPA improvement, 2 engineers, 4-week sprint, 560 hours. This contradicts every architect-reviewed document.

4. **CONSOLIDATE-COMPLETION.md reports 30.7s CSPA time** (line 98), but all other documents use 28.7s. The baseline number is inconsistent. (The 28.7s appears to be from a different run or configuration.)

---

## Missing Opportunities

### Paths Not Explored

1. **Radix sort for int64_t rows.** Consolidate uses `qsort_r` (comparison-based, O(n log n)). For fixed-width int64_t keys, an MSB radix sort achieves O(n * w) where w = number of columns. For typical column counts (2-4), this could be significantly faster than qsort on large relations. Not analyzed.

2. **Lazy/deferred consolidation.** Currently every iteration consolidates every relation. If a relation's output is only consumed by one join in the next iteration, consolidation could be deferred until the join needs sorted input. This would eliminate unnecessary sorts for relations that are still accumulating rows.

3. **Stratum-level parallelism.** The documents focus exclusively on K-copy parallelism within a single stratum. Independent strata could be evaluated in parallel with zero correctness risk (they have no data dependencies by definition). This is orthogonal to K-fusion and could compound with it.

4. **Column-oriented sort.** The current row-major layout requires full-row comparisons. A column-major layout with per-column sort + merge could improve cache locality for the sort phase. This is a larger architectural change but worth mentioning as a long-term path.

5. **Join algorithm alternatives.** No document analyzes whether the current join implementation is optimal. Hash joins vs sort-merge joins have different performance characteristics depending on relation sizes and selectivity. The join phase accounts for 12% of wall time -- non-trivial.

---

## Quality Audit Verdict

| Criterion | Rating |
|-----------|--------|
| Root cause identification | GOOD -- K-copy redundancy correctly identified |
| Solution selection process | ADEQUATE -- correct choice made, but alternatives insufficiently explored |
| Internal consistency | POOR -- multiple contradictions between pre/post architect documents |
| Evidence quality | WEAK -- profiling claims lack tool output backing |
| Implementation readiness | NEEDS WORK -- memcmp bug in merge code, stale design docs |
| Risk assessment | GOOD -- day-3 gates are well-designed |
| Test strategy | NEEDS WORK -- does not cover known bug class (memcmp on int64_t) |
