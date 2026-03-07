# CSPA Validation Results

**Status**: PENDING — awaiting US-003~006 (CSE infrastructure) completion

---

## Overview

CSPA (Context-Sensitive Points-to Analysis) is the primary correctness and performance
validation workload for Option 2 (CSE materialization with K-atom multi-way delta expansion).

**Correctness oracle**: output tuple count MUST equal **20,381** (baseline from `benchmark-baseline-2026-03-07.md`).

---

## How to Run

```bash
# Build first
meson compile -C build

# US-007: Correctness validation
./scripts/run_cspa_validation.sh

# US-007 with explicit repeat
./scripts/run_cspa_validation.sh --repeat 3

# US-008: Performance tuning run (single repeat for speed)
./scripts/run_cspa_validation.sh --repeat 1
```

---

## Baseline (Pre-Option-2)

From `benchmark-baseline-2026-03-07.md`, branch `next/pure-c11` before CSE:

| Metric | Baseline Value |
|---|---|
| Output tuples | 20,381 |
| Min wall time | 4,422 ms |
| Median wall time | 4,602 ms |
| Max wall time | 4,644 ms |
| Peak RSS | 4,670,592 KB (~4.46 GB) |
| Repeat | 3 |

**Root causes of baseline poor performance** (from CSPA deep-profile analysis):
- O(N log N) re-sort of ALL accumulated rows every semi-naive iteration
- Incomplete delta expansion: multi-atom rules generate full cross-products instead of K-way delta joins
- Memory amplification ~14,000x: `old_data` snapshots × 3 relations per iteration, plus join hash tables

---

## US-007 Results (Correctness Validation)

**Date**: _TBD_
**Branch**: _TBD_
**Git SHA**: _TBD_

### Run Command
```bash
./scripts/run_cspa_validation.sh --repeat 3
```

### Raw TSV Output
```
# Paste TSV output here
```

### Metrics

| Metric | Value | vs Baseline | Pass? |
|---|---|---|---|
| Output tuples | _TBD_ | must = 20,381 | _TBD_ |
| Iteration count | _TBD_ | record only | N/A |
| Min wall time (ms) | _TBD_ | _TBD_ | N/A |
| Median wall time (ms) | _TBD_ | _TBD_ | N/A |
| Peak RSS (KB) | _TBD_ | must be <= 4,670,592 | _TBD_ |

### Correctness Verdict
- [ ] PASS: Tuple count == 20,381
- [ ] FAIL: Tuple count mismatch — investigate delta_mode regression

### Notes
_TBD_

---

## US-008 Results (Performance Optimization)

**Date**: _TBD_
**Branch**: _TBD_
**Git SHA**: _TBD_

### Hypothesis Targets

From `HYPOTHESIS-VALIDATION.md` / `OPTIMIZATION-STRATEGY.md`:

| Hypothesis | Target | Metric |
|---|---|---|
| H2: Iteration reduction via K-way delta | >20% fewer iterations | Iteration count |
| Memory savings via CSE materialization | >=30% reduction vs baseline | Peak RSS KB |
| Wall time improvement | any reduction | Median ms |

### Tuning Parameters Explored

| Parameter | Value Tested | Median ms | Peak RSS KB | Notes |
|---|---|---|---|---|
| _baseline (no CSE)_ | — | 4,602 | 4,670,592 | Pre-Option-2 |
| memory_limit default | _TBD_ | _TBD_ | _TBD_ | |
| memory_limit reduced | _TBD_ | _TBD_ | _TBD_ | |
| materialization threshold | _TBD_ | _TBD_ | _TBD_ | |

### Best Configuration Found

```
# Document winning CSE parameter values here
```

| Metric | Baseline | With Option 2 | Delta | % Change |
|---|---|---|---|---|
| Median wall time (ms) | 4,602 | _TBD_ | _TBD_ | _TBD_ |
| Peak RSS (KB) | 4,670,592 | _TBD_ | _TBD_ | _TBD_ |
| Iteration count | ~100-300 (est.) | _TBD_ | _TBD_ | _TBD_ |

### H2 Hypothesis Confirmation

- [ ] H2 CONFIRMED: iteration count reduced >20%
- [ ] H2 PARTIAL: some reduction but below 20% target
- [ ] H2 REFUTED: no iteration reduction observed

### Memory Savings

- [ ] Target met: >=30% peak RSS reduction
- [ ] Target missed: < 30% reduction

### Performance Verdict

_TBD_

### Notes on Tuning

_TBD_

---

## Commit Record

| Story | Commit | Message |
|---|---|---|
| US-007 | _TBD_ | `test: validate Option 2 correctness on CSPA` |
| US-008 | _TBD_ | `perf: tune CSE materialization thresholds for CSPA` |
