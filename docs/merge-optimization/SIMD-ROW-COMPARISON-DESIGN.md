# SIMD Row Comparison Optimization Design

## Overview

Optimize `col_op_consolidate_incremental_delta` merge bottleneck by replacing scalar `row_cmp_lex()` with SIMD-accelerated comparison that supports both AVX2 (x86-64) and NEON (ARM64).

## Problem

Current merge loop (lines 1570-1610 in columnar_nanoarrow.c):
```c
while (oi < old_nrows && di < d_unique) {
    const int64_t *orow = rel->data + (size_t)oi * nc;
    const int64_t *drow = delta_start + (size_t)di * nc;
    int cmp = row_cmp_lex(orow, drow, nc);  // ← O(ncols) per iteration
    // ...memcpy(merged + ..., orow, row_bytes);
    // ...
}
```

**Bottlenecks:**
1. **row_cmp_lex** iterates ncols times per comparison
2. **Branch misprediction** in the comparison loop
3. **Pointer arithmetic** repeated per iteration
4. **memcpy** called per row (memory bandwidth bound)

**Target:** Reduce comparison time by 40-50% using SIMD parallelism.

## Solution Architecture

### 1. SIMD Row Comparison Functions

#### AVX2 Implementation: `row_cmp_simd_avx2()`

**Platform:** x86-64 with AVX2 support (`__AVX2__`)

**Algorithm:**
- Compare 4 int64_t elements in parallel per iteration
- Use `_mm256_cmpeq_epi64` (equality) and `_mm256_cmpgt_epi64` (greater-than)
- Extract first differing element using SIMD reduction
- Fallback loop for remainder (ncols % 4)

**Signature:**
```c
static inline int
row_cmp_simd_avx2(const int64_t *a, const int64_t *b, uint32_t ncols)
{
    // Compare ncols elements in groups of 4
    // Return -1, 0, or 1 (same semantics as row_cmp_lex)
}
```

**Complexity:**
- Time: O(ncols / 4) with fallback O(ncols % 4)
- Speedup: ~4× for large ncols

#### NEON Implementation: `row_cmp_simd_neon()`

**Platform:** ARM64 with NEON support (`__ARM_NEON__`)

**Algorithm:**
- Compare 2 int64_t elements in parallel per iteration
- Use `vceqq_s64` (equality) and `vcgtq_s64` (greater-than)
- Extract first differing element using SIMD reduction
- Fallback loop for remainder (ncols % 2)

**Signature:**
```c
static inline int
row_cmp_simd_neon(const int64_t *a, const int64_t *b, uint32_t ncols)
{
    // Compare ncols elements in groups of 2
    // Return -1, 0, or 1 (same semantics as row_cmp_lex)
}
```

**Complexity:**
- Time: O(ncols / 2) with fallback O(ncols % 2)
- Speedup: ~2× for large ncols

### 2. Dispatcher: `row_cmp_optimized()`

**Purpose:** Select best implementation at compile time

**Logic:**
```c
#ifdef __AVX2__
    #define row_cmp_optimized row_cmp_simd_avx2
#elif defined(__ARM_NEON__)
    #define row_cmp_optimized row_cmp_simd_neon
#else
    #define row_cmp_optimized row_cmp_lex
#endif
```

**Fallback:** Seamlessly use `row_cmp_lex` if neither SIMD is available.

### 3. Integration Points

**Current code (line 1574):**
```c
int cmp = row_cmp_lex(orow, drow, nc);
```

**After optimization:**
```c
int cmp = row_cmp_optimized(orow, drow, nc);
```

## Correctness Strategy

### Functional Equivalence
- All SIMD implementations must return identical results to `row_cmp_lex()`
- Fallback for remainder elements (ncols % SIMD_WIDTH) must be bit-identical

### Testing
- TDD: Write tests BEFORE implementation
- Compare SIMD result vs scalar for random row pairs
- Edge cases: ncols=1, ncols=SIMD_WIDTH-1, ncols=large prime

### Verification
- All 15 workloads produce identical output before/after
- CSPA: 20,381 tuples exact match
- No undefined behavior (alignment, overflow)

## Optimization Strategy: Pointer Caching

**Current pattern (repeated per loop iteration):**
```c
const int64_t *orow = rel->data + (size_t)oi * nc;
const int64_t *drow = delta_start + (size_t)di * nc;
```

**Optimized pattern:**
```c
int64_t *o_ptr = rel->data;
int64_t *d_ptr = delta_start;
// ...
const int64_t *orow = o_ptr + (size_t)oi * nc;
const int64_t *drow = d_ptr + (size_t)di * nc;
// ...
o_ptr += nc; // increment instead of recalculate
```

**Expected gain:** 5-10% reduction in pointer arithmetic instructions.

## memcpy Minimization

**Current approach:** memcpy entire row per iteration

**Options:**
1. **Pointer swap** (preferred): Use pointers directly instead of copying
2. **Batch copying**: Copy multiple rows at once
3. **Ring buffer**: Minimize allocation overhead

**Initial strategy:** Evaluate Option 1 (pointer swaps) for correctness/performance tradeoff.

## Performance Expectations

| Optimization | Expected Speedup | Rationale |
|---|---|---|
| SIMD row comparison (AVX2) | 3-4× | Parallel comparison of 4 int64_t |
| Pointer caching | 1.05-1.1× | Reduce pointer arithmetic |
| memcpy reduction | 1.1-1.2× | Lower memory bandwidth pressure |
| Combined | 1.15-1.25× | 15-25% overall improvement |

## Implementation Phases

**Phase 1: SIMD Implementations**
- US-003: AVX2 implementation
- US-004: NEON implementation
- US-005: Dispatcher + conditional compilation

**Phase 2: Merge Loop Optimization**
- US-006: Pointer caching + memcpy reduction
- US-007: Correctness validation

**Phase 3: Verification**
- US-008: Performance measurement
- US-009: Architect review

## Dependencies

- No new external libraries
- Compile-time feature detection only
- Fallback to scalar C code for unsupported platforms
- C11 inline assembly (if needed for advanced SIMD)

## Files to Modify

1. `wirelog/backend/columnar_nanoarrow.c`
   - Add `row_cmp_simd_avx2()`
   - Add `row_cmp_simd_neon()`
   - Add `row_cmp_optimized()` dispatcher
   - Refactor `col_op_consolidate_incremental_delta()` merge loop

2. `wirelog/backend/columnar_nanoarrow.h` (optional)
   - Export dispatcher if used elsewhere

## Compiler Flags

**AVX2 compilation:**
```bash
gcc -mavx2 ... (already in meson.build if configured)
```

**NEON compilation:**
```bash
gcc -mfpu=neon -march=armv8-a ...
```

**Fallback:**
```bash
gcc ... (no special flags, uses scalar code)
```

## Risk Assessment

**Low risk:**
- SIMD code isolated in static inline functions
- Dispatcher uses macro substitution (zero runtime overhead)
- Fallback path identical to current code
- All 15 workloads must produce identical output (gating correctness)

**Testing coverage:**
- Unit tests for SIMD functions (before integration)
- Full benchmark suite (15 workloads)
- Architectural review (scalar vs SIMD semantics)
