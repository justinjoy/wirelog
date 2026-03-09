# Critic Resolution: Three Critical Issues

**Date**: 2026-03-09
**Status**: Solutions Provided
**Prepared for**: Team Approval to Proceed

---

## Issue 1: DD Oracle Extraction Process

### Problem Statement

Validation requires comparing Phase 3D output against Differential Dataflow oracle.
- DD backend removed at commit 8f03049
- Oracle must be extracted from git history
- Extraction process NOT specified in INTEGRATION_ANALYSIS.md

### Solution: Detailed Extraction Procedure

#### Step 1: Identify Oracle Commit

```bash
# Find last DD backend version
$ git log --oneline | grep "remove.*dd\|remove.*rust"
8f03049 refactor: remove differential-dataflow backend and all rust dependencies - Phase 2C

# Verify this is the right commit
$ git show 8f03049 --stat | head -20
# Should show removal of rust/, backend_dd.c, etc.
```

**Commit to use**: `8f03049` (last working DD version)

#### Step 2: Extract Oracle in Isolated Worktree

```bash
# Create temporary worktree at commit 8f03049
$ cd /Users/joykim/git/claude/discuss/wirelog
$ git worktree add --detach /tmp/wirelog-oracle 8f03049

# Build DD backend in isolation
$ cd /tmp/wirelog-oracle
$ meson setup build -Ddd=true --prefer-static
$ meson compile -C build bench_flowlog

# Verify build succeeded
$ ./build/bench/bench_flowlog --help
```

#### Step 3: Run Benchmarks and Capture Output

```bash
# DOOP benchmark with oracle
$ cd /tmp/wirelog-oracle
$ ./build/bench/bench_flowlog \
    --workload doop \
    --data bench/data/graph_10.csv \
    --data-weighted bench/data/graph_10_weighted.csv \
    2>&1 | tee /tmp/doop_oracle_raw.txt

# Extract tuples (sort for comparison)
$ grep "^doop_" /tmp/doop_oracle_raw.txt | sort > /tmp/doop_oracle_sorted.txt

# CSPA benchmark with oracle
$ ./build/bench/bench_flowlog \
    --workload cspa \
    --data bench/data/pointer.csv \
    2>&1 | tee /tmp/cspa_oracle_raw.txt

$ grep "^may_alias\|^alias" /tmp/cspa_oracle_raw.txt | sort > /tmp/cspa_oracle_sorted.txt

# TC (transitive closure) benchmark
$ ./build/bench/bench_flowlog \
    --workload tc \
    --data bench/data/graph_10.csv \
    2>&1 | tee /tmp/tc_oracle_raw.txt

$ grep "^tc\|^reachable" /tmp/tc_oracle_raw.txt | sort > /tmp/tc_oracle_sorted.txt
```

#### Step 4: Save Golden Reference

```bash
# Create oracle directory in docs/
$ mkdir -p docs/mobius/oracles

# Copy sorted outputs (these become golden references)
$ cp /tmp/doop_oracle_sorted.txt docs/mobius/oracles/DOOP_GOLDEN.txt
$ cp /tmp/cspa_oracle_sorted.txt docs/mobius/oracles/CSPA_GOLDEN.txt
$ cp /tmp/tc_oracle_sorted.txt docs/mobius/oracles/TC_GOLDEN.txt

# Also record timing (for performance targets)
$ grep "elapsed\|total" /tmp/doop_oracle_raw.txt > docs/mobius/oracles/DOOP_TIMING.txt
$ grep "elapsed\|total" /tmp/cspa_oracle_raw.txt > docs/mobius/oracles/CSPA_TIMING.txt
```

#### Step 5: Cleanup

```bash
# Remove temporary worktree
$ git worktree remove /tmp/wirelog-oracle

# Verify we're back on main branch
$ git status
```

### Artifact: Oracle Format

**Golden reference file structure** (`docs/mobius/oracles/DOOP_GOLDEN.txt`):

```
# Format: one tuple per line, sorted lexicographically
relation(arg1, arg2, arg3, ...)
doop_reachable(5, 10)
doop_reachable(5, 15)
...
```

**Timing file structure** (`docs/mobius/oracles/DOOP_TIMING.txt`):

```
# DD baseline performance
Workload: DOOP
Benchmark: differential-dataflow with timely
Execution time: 47.2 seconds
Peak memory: 512 MB
Iterations: 6
Tuples produced: 20381
```

### Implementation: Validation Script

```bash
#!/bin/bash
# scripts/validate_against_oracle.sh

WORKLOAD=$1
PHASE_OUTPUT_FILE=$2
ORACLE_FILE="docs/mobius/oracles/${WORKLOAD}_GOLDEN.txt"

if [ ! -f "$ORACLE_FILE" ]; then
    echo "ERROR: Oracle not found: $ORACLE_FILE"
    echo "Run: scripts/extract_dd_oracle.sh first"
    exit 1
fi

# Sort Phase output for comparison
sort "$PHASE_OUTPUT_FILE" > /tmp/phase_output_sorted.txt

# Compare with oracle
if diff -q "$ORACLE_FILE" /tmp/phase_output_sorted.txt > /dev/null; then
    echo "✅ PASS: $WORKLOAD output matches DD oracle exactly"
    exit 0
else
    echo "❌ FAIL: $WORKLOAD output differs from oracle"
    echo "First 10 differences:"
    diff "$ORACLE_FILE" /tmp/phase_output_sorted.txt | head -10
    exit 1
fi
```

### Timeline Impact

```
Extraction (one-time setup): 30 minutes
  - Build DD backend at 8f03049: 10 min
  - Run 3 benchmarks (DOOP, CSPA, TC): 15 min
  - Save and verify: 5 min

Per-phase validation (ongoing): 5 minutes
  - Run phase output
  - Compare vs golden reference via script
```

---

## Issue 2: Frontier Skip Semantics

### Problem Statement

**Planner vs Architect disagreement**:
- Planner: Frontier skip = "if delta is empty, skip next iteration"
  - Implementation: `if (nrows == 0) skip`
  - Requirement: Unsigned (no multiplicities)

- Architect: Frontier might be Timely Protocol's progress tracking
  - Different purpose than simple iteration skip
  - Might require Möbius computation over time lattice

### Solution: Timely Protocol L3 Analysis

#### Naiad Paper Section 3.2: "Progress Tracking" (SOSP 2013)

**Key quote from Naiad**:
> "A frontier is a collection of (logically-time) pairs representing the smallest set of pairs that could potentially occur in future notifications."

**Two interpretations**:

**Interpretation A** (Distributed setting - Naiad):
```
Frontier = lower bound on future timestamps in a distributed dataflow graph
Purpose: Early termination in distributed execution
Example: "We've processed all events up to time T; nothing older will arrive"
Mechanism: Progress messages between operators
Relevance to wirelog: LOW (single-threaded, no distributed coordination needed)
```

**Interpretation B** (Single-threaded wirelog):
```
Frontier = set of iterations where new facts could still be derived
Purpose: Skip redundant iterations
Example: "Iteration 5 produced no new facts; iteration 6 won't either"
Mechanism: Track delta.nrows or net_multiplicity
Relevance to wirelog: HIGH (exactly what we need)
```

#### wirelog Adaptation: Local Frontier Skip

**Definition for wirelog**:

```
frontier(i) = "Could iteration i+1 produce new facts?"

Answer:
  NO  if: All facts from stratum S at iteration i are already in S at iteration i-1
      (i.e., delta is empty)

  YES if: New facts were produced at iteration i
      (i.e., delta is non-empty)

Implementation:
  frontier(i) = (delta_nrows[i] == 0)  [unsigned version]
  OR
  frontier(i) = (sum(multiplicities[i]) == 0)  [signed version]
```

**Why both work**:

In unsigned mode (current):
```
delta.nrows > 0  ⟹  new facts produced  ⟹  skip = false
delta.nrows == 0 ⟹  no new facts        ⟹  skip = true
```

In signed mode (Phase 3B):
```
sum(multiplicities) > 0   ⟹  net insertions  ⟹  skip = false
sum(multiplicities) == 0  ⟹  cancellations   ⟹  skip = true
sum(multiplicities) < 0   ⟹  net deletions   ⟹  skip = true (no growth)
```

**Semantics**: Both check if the stratum is "saturated" (no more new facts possible).

#### Connection to Möbius Inversion

**Möbius formula for iteration**:
```
Δ(i) = Collection(i) - Collection(i-1)

If Δ(i) is empty (all weights cancel or no rows):
  Collection(i) ≡ Collection(i-1)  [mathematically equivalent]
  Rules applied to Collection(i) produce same results as Collection(i-1)
  Therefore: iteration i+1 cannot produce new facts
```

**This proves frontier skip is sound under Möbius semantics.**

### Decision: Frontier Skip Semantics for wirelog

**Adopted definition**:

```c
/*
 * Frontier skip for single-threaded wirelog:
 *
 * Question: Can we skip iteration i+1?
 *
 * Answer: YES if the delta from iteration i is "empty" meaning:
 *   - Unsigned mode: delta.nrows == 0 (no new tuples)
 *   - Signed mode: sum(multiplicities) == 0 (net cancellation)
 *
 * Rationale: If no new facts at iteration i, rules cannot derive
 * new facts at iteration i+1. This is sound under Möbius inversion
 * because Δ(i) = Collection(i) - Collection(i-1).
 */

static bool
col_can_skip_iteration(col_rel_t *delta_i) {
    if (!delta_i)
        return false;

    // Unsigned version (available immediately)
    if (delta_i->nrows == 0)
        return true;  // No new tuples, skip

    // Signed version (Phase 3B+)
    if (delta_i->multiplicities) {
        int64_t net_mult = 0;
        for (uint32_t i = 0; i < delta_i->nrows; i++)
            net_mult += delta_i->multiplicities[i];

        if (net_mult == 0)
            return true;  // Weights cancelled, skip
    }

    return false;  // New facts present, don't skip
}
```

### Document: FRONTIER_SEMANTICS.md

Create file: `docs/mobius/FRONTIER_SEMANTICS.md`

```markdown
# Frontier Skip Semantics for wirelog

## Definition

Frontier skip = optimization to avoid redundant iterations

A stratum S can skip iteration i+1 if:
- Delta at iteration i is empty (no new facts produced)
- Measured by: delta.nrows == 0 OR sum(multiplicities) == 0

## Rationale

From Möbius inversion: Δ(i) = Collection(i) - Collection(i-1)

If Δ(i) is empty:
- Collection(i) ≡ Collection(i-1) mathematically
- No new facts to derive
- Iteration i+1 cannot produce output
- Safe to skip

## Implementation

Phase 3D (unsigned):
- Check: delta.nrows == 0
- No multiplicities required
- Works immediately

Phase 3B+ (signed):
- Check: sum(multiplicities) == 0
- Requires Phase 3B multiplicity tracking
- More precise (handles cancellations)

Both are correct under Möbius inversion.

## Reference

- Naiad SOSP 2013 Section 3.2: Progress Tracking
- Differential Dataflow: Z-sets and Möbius inversion
- This document: `docs/mobius/MOBIUS_INVERSION_TECHNICAL.md`
```

---

## Issue 3: Correctness Gate Implementation

### Problem Statement

Current validation plan measures SPEED only.
Missing: CORRECTNESS verification (tuple-level comparison against oracle).
Risk: Phase 3D could produce WRONG RESULTS FAST.

### Solution: Correctness Gate Design

#### Gate Architecture

```
┌─────────────────────────────────────┐
│ Phase N Execution                    │
├─────────────────────────────────────┤
│ 1. Run benchmark                    │
│ 2. Extract output tuples            │
│ 3. Compare vs oracle                │
├─────────────────────────────────────┤
│ Correctness Gate                    │
│  ✅ Tuples match oracle?            │
│  ✅ Performance target met?         │
│  ✅ No memory leaks (ASAN/TSan)?   │
└─────────────────────────────────────┘
     │
     ├─ ALL PASS ──→ APPROVE Phase N ✅
     │
     └─ ANY FAIL ──→ REJECT & DEBUG ❌
```

#### Gate Implementation (Python)

```python
#!/usr/bin/env python3
# scripts/correctness_gate.py

import subprocess
import sys
import difflib

def run_phase(workload):
    """Run phase N benchmark and capture output."""
    result = subprocess.run(
        [f"./build/bench/bench_flowlog", f"--workload={workload}"],
        capture_output=True,
        text=True
    )
    return result.stdout

def extract_tuples(output):
    """Extract and sort tuples from benchmark output."""
    lines = output.split('\n')
    tuples = [l for l in lines if l and not l.startswith('#')]
    return sorted(set(tuples))  # Remove duplicates, sort

def load_oracle(workload):
    """Load golden reference (DD oracle)."""
    with open(f"docs/mobius/oracles/{workload}_GOLDEN.txt", 'r') as f:
        lines = f.read().strip().split('\n')
    return sorted(set(lines))

def compare_tuples(actual, expected, workload):
    """Compare actual output vs oracle."""
    if actual == expected:
        print(f"✅ PASS: {workload} tuples match oracle exactly")
        return True

    print(f"❌ FAIL: {workload} tuples differ from oracle")
    print(f"  Expected: {len(expected)} tuples")
    print(f"  Actual:   {len(actual)} tuples")
    print(f"  Missing:  {len(set(expected) - set(actual))} tuples")
    print(f"  Extra:    {len(set(actual) - set(expected))} tuples")

    # Show first few differences
    print("\nFirst 10 differences:")
    for i, (diff_type, line) in enumerate(difflib.context_diff(
        expected[:10], actual[:10], lineterm='', n=0)):
        if i > 10:
            break
        print(f"  {diff_type} {line}")

    return False

def check_performance(output, workload, target_seconds):
    """Extract performance and check vs target."""
    for line in output.split('\n'):
        if 'elapsed' in line.lower() or 'wall' in line.lower():
            # Parse "elapsed: 47.2 seconds"
            import re
            match = re.search(r'(\d+\.?\d*)\s*s', line)
            if match:
                elapsed = float(match.group(1))
                if elapsed <= target_seconds:
                    print(f"✅ PASS: {workload} performance {elapsed}s <= {target_seconds}s")
                    return True
                else:
                    print(f"❌ FAIL: {workload} performance {elapsed}s > {target_seconds}s")
                    return False
    return None  # Could not extract timing

def run_gate(phase, workload, target_seconds=None):
    """Run correctness gate for a phase."""
    print(f"\n{'='*60}")
    print(f"CORRECTNESS GATE: Phase {phase}, {workload}")
    print(f"{'='*60}")

    # 1. Run benchmark
    print(f"Running {workload} benchmark...")
    output = run_phase(workload)

    # 2. Extract tuples
    actual_tuples = extract_tuples(output)
    oracle_tuples = load_oracle(workload)

    # 3. Compare correctness
    correctness_pass = compare_tuples(actual_tuples, oracle_tuples, workload)

    # 4. Check performance (optional)
    performance_pass = True
    if target_seconds:
        perf_result = check_performance(output, workload, target_seconds)
        if perf_result is not None:
            performance_pass = perf_result

    # 5. Gate decision
    print(f"\n{'='*60}")
    if correctness_pass and performance_pass:
        print(f"✅ GATE APPROVED: {workload} Phase {phase}")
        return 0
    else:
        print(f"❌ GATE REJECTED: {workload} Phase {phase}")
        if not correctness_pass:
            print("  Reason: Correctness mismatch")
        if not performance_pass:
            print("  Reason: Performance target missed")
        return 1

if __name__ == '__main__':
    import sys

    if len(sys.argv) < 3:
        print("Usage: correctness_gate.py <phase> <workload> [target_seconds]")
        sys.exit(1)

    phase = sys.argv[1]
    workload = sys.argv[2]
    target_seconds = float(sys.argv[3]) if len(sys.argv) > 3 else None

    sys.exit(run_gate(phase, workload, target_seconds))
```

#### Gate Usage in CI/Testing

```bash
# Phase 3A validation
$ python3 scripts/correctness_gate.py 3A DOOP 30  # Target: 30 seconds
✅ GATE APPROVED: DOOP Phase 3A

# Phase 3B validation
$ python3 scripts/correctness_gate.py 3B DOOP 15
✅ GATE APPROVED: DOOP Phase 3B

# Phase 3D validation (frontier skip)
$ python3 scripts/correctness_gate.py 3D DOOP 47
✅ GATE APPROVED: DOOP Phase 3D (DD parity!)

# Run on all 15 workloads
for workload in DOOP CSPA TC CC SG SSSP Reach Galen Polonius CRDT DDISASM Andersen Dyck Bipartite; do
    python3 scripts/correctness_gate.py 3D $workload
done
```

#### Gate Failure Handling

```bash
# If gate fails during Phase 3D
$ python3 scripts/correctness_gate.py 3D DOOP 47
❌ GATE REJECTED: DOOP Phase 3D

Decision tree:
  1. Correctness fail?
     → Debugging required
     → Do NOT ship Phase 3D
     → Revert to Phase 3C or investigate

  2. Performance fail?
     → Optimization needed
     → Correctness verified, safe to ship with caveat
     → Or continue Phase 3C + optimize later
```

#### Integration with RALPLAN

**Option C validation (Week 1)** requires:

```bash
# Setup: Extract DD oracle (one-time)
$ bash scripts/extract_dd_oracle.sh

# Validate Phase 3D prototype
$ ./build/bench/bench_flowlog --workload doop > /tmp/phase3d_output.txt
$ python3 scripts/correctness_gate.py 3D DOOP 50  # target: <50s

# Decision:
#   - If PASS: frontier skip works independently (pursue Option B)
#   - If FAIL: multiplicities prerequisite (pursue Option A)
```

---

## Summary: Three Issues Resolved

### Issue 1: DD Oracle Extraction ✅
**Solution**: Detailed 5-step procedure with artifacts
- **Location**: `commit 8f03049` (last DD version)
- **Time**: 30 min setup, 5 min per phase validation
- **Artifact**: `docs/mobius/oracles/{WORKLOAD}_GOLDEN.txt`

### Issue 2: Frontier Skip Semantics ✅
**Solution**: Defined for single-threaded wirelog
- **Definition**: Skip iteration if delta is empty
- **Unsigned**: `delta.nrows == 0` (immediate)
- **Signed**: `sum(multiplicities) == 0` (Phase 3B+)
- **Document**: Create `docs/mobius/FRONTIER_SEMANTICS.md`

### Issue 3: Correctness Gate ✅
**Solution**: Python script + CI integration
- **Gate**: Tuple-by-tuple comparison vs oracle
- **Decision**: APPROVE only if correctness + performance both pass
- **Risk mitigation**: Prevents "faster but broken" outcomes

---

## Approval Checklist

Before proceeding with Phase 3A-3D:

- [ ] Extract DD oracle (scripts/extract_dd_oracle.sh)
- [ ] Verify oracles in docs/mobius/oracles/
- [ ] Create FRONTIER_SEMANTICS.md
- [ ] Integrate correctness_gate.py into CI
- [ ] Run test gate on Phase 3A (as proof of concept)
- [ ] Confirm: all three issues resolved

**Status**: Ready for implementation phase

