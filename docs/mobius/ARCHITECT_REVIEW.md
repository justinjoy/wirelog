# Architect Review: Integration Analysis

**Date**: 2026-03-09
**Reviewer**: Architect (Structural Soundness)
**Review of**: INTEGRATION_ANALYSIS.md (Planner draft)

---

## Structural Assessment

### ✅ STRONG POINTS

#### 1. Correct Problem Framing
Planner correctly identified the **central uncertainty**:
- "Is frontier skip independent of multiplicity tracking?"
- This is the KEY architectural question that determines feasibility
- Not asked in previous analyses → This is novel contribution

**Strength**: Reframes 6-week plan vs 3-week plan decision as empirical question
**Impact**: HIGH (could save 3 weeks if Phase 3D proves independent)

#### 2. Valid Option Space
Three options are genuinely distinct:

- **Option A**: Sequential (conventional, safe, matches original analysis)
- **Option B**: Aggressive frontier-first (risky but potentially faster)
- **Option C**: Validation-first (recommended, de-risks the project)

**Strength**: Not false dichotomies; real tradeoffs exist
**Soundness**: Each option is internally consistent
**Decision criteria**: Clear (empirical validation of frontier skip)

#### 3. Dependency Analysis is Testable
Planner's core claim:

> "Unsigned deltas (nrows > 0) ≈ signed net multiplicity != 0"

This can be **validated in 1 week** via prototype:
- Implement two frontier skip versions (unsigned vs signed)
- Run on same benchmark suite
- Measure: identical results? identical performance?

**Strength**: Makes architectural assumption explicit and testable
**Validity**: This is how architecture decisions should be validated

---

### ⚠️ CONCERNS & GAPS

#### 1. **CRITICAL ASSUMPTION UNCHALLENGED**

Planner assumes: "Frontier skip = checking if delta is empty"

But Timely Protocol L3 (from the article) says:
> "frontier: per-operator lower bound on future timestamps"

**Gap**: Planner doesn't distinguish between:
- **A) Frontier as optimization**: "Can we skip this stratum?"
- **B) Frontier as progress invariant**: "What's the lower bound on unprocessed timestamps?"

These are DIFFERENT concepts:
- (A) is what wirelog needs for performance
- (B) is what Timely uses for distributed correctness

**Risk**: If (B) is the actual spec, frontier skip might be a red herring.
Timely Protocol L3 might not be about skipping iterations at all.

**Example**:
```
Timely's frontier tracks: "Nothing at timestamp < X will arrive"
This enables: Early termination in distributed setting
But for single-threaded wirelog: Not needed for correctness

Frontier skip (wirelog's use): "Stratum S produced no output at iteration i-1"
This enables: Skip stratum S at iteration i
Different purpose than Timely's progress tracking
```

**Architect recommendation**: Validate what Timely Protocol L3 actually means for single-threaded execution

---

#### 2. **Three Articles Tension Unresolved**

**Arrow-for-Timely** (L4):
- Says: "delta batch layer handles both bulk and incremental"
- Implies: Multiplicities needed for incremental (record-level tracking)

**Timely Protocol** (L3):
- Says: "progress tracking via frontier"
- Implies: Maybe multiplicities needed for frontier computation?
- But: Doesn't explicitly say

**Differential Dataflow**:
- Says: "Z-sets = (value, multiplicity) pairs"
- Implies: Multiplicities essential for ANY correct incremental computation

**Tension**: Do multiplicities serve (A) correctness only, or (B) performance too?

Planner's answer: "Correctness (3B-3C), performance can use unsigned (3D)"

**Architect's question**: Is this actually true?
- If DOOP uses recursive COUNT, unsigned deltas will produce WRONG RESULTS
- Then Phase 3D frontier skip on wrong results → speeds up wrong computation
- That's not "faster", that's "faster-and-broken"

**Risk**: DOOP might REQUIRE multiplicities for correctness, not just optimization

---

#### 3. **Validation Plan is Incomplete**

Planner proposes 1-week validation with 4 tasks, but:

**Missing**:
- How do we validate Phase 3D's CORRECTNESS?
  - Just checking row counts (unsigned) might give wrong results
  - Need: compare output tuples against DD oracle
  - Current plan only measures speed, not accuracy

- What's the success criterion for "Phase 3D alone works"?
  - 47s on DOOP? (performance target)
  - Correct tuples vs DD oracle? (correctness target)
  - Both? (required)

**Architect concern**: If Phase 3D produces wrong results but fast, we've failed.
The validation plan should require tuple-level correctness verification against DD oracle.

**Recommendation**: Add DD oracle comparison to validation tasks.

---

#### 4. **The "Multiphase Correctness" Paradox**

If multiplicities are truly needed for correctness:

```
Phase 3B (multiplicities) + Phase 3C (joins) = CORRECT
Phase 3B (multiplicities) + no Phase 3C = PARTIALLY CORRECT
Phase 3B (multiplicities) + only frontier = PARTIALLY CORRECT
Phase 3D (frontier) + no Phase 3B = INCORRECT
```

Then proceeding with Option B (3A → 3D only) is dangerous:
- Might mask correctness issues with performance improvements
- Hard to debug later when multiplicities finally added

**Alternative interpretation**:
```
Phase 3A (K-fusion) = CORRECT (just parallel)
Phase 3B (multiplicities) = CORRECT (just accounting)
Phase 3C (weighted joins) = CORRECT (propagation)
Phase 3D (frontier) = CORRECT (optimization)

But also:
Phase 3D without 3B = Maybe still correct? (hypothesis to test)
```

**Architect's stand**: This needs clarification. The validation plan should prove whether Phase 3D can be sound without Phase 3B.

---

## Steelman Antithesis: The Case for Option A

**Planner favors Option C** (validate first), suggesting Option B might work.

**Steelman for Option A** (sequential, 6 weeks):

"Correctness is non-negotiable (P1). Differential Dataflow's Z-sets are the mathematical foundation. Trying to skip Phase 3B for speed is premature optimization.

DD doesn't have a 'fast path' that omits multiplicities. EVERY operator propagates multiplicities:
- JOIN: mult = mult_L × mult_R
- FILTER: mult unchanged
- AGGREGATE: value += mult × input_value

If wirelog omits Phase 3B, we're not implementing Differential Dataflow correctly.

Yes, frontier skip might work without Phase 3B. But it's working on incorrect data. The 1-week validation risk is real: Phase 3D might appear to work (fast) while producing wrong results (correctness failure discovered after investment).

Better: Implement 3B-3C first (guaranteed correct), then 3D as optimization. Takes 6 weeks, but delivers correct +fast result."

**This is a valid position.** Option A has lower risk of correctness failure.

---

## Real Tradeoff Tension

The actual tension is between:

| Axis | Option A | Option B |
|------|----------|----------|
| Correctness risk | LOW (multiplicities first) | HIGH (frontier without them) |
| Time to frontier | 5+ weeks | 1 week |
| Complexity | 3,000 LOC | 1,500 LOC |
| Testability | Easy (each phase independent) | Hard (frontier semantics unclear) |
| ROI curve | Smooth (each phase adds value) | Steep (frontier makes all difference) |

**Synthesis**: Option C (validation-first) is a true hybrid:
- Test frontier skip's feasibility BEFORE committing to 6-week path
- Risk: 1 week lost if validation fails
- Reward: 3 weeks saved if validation succeeds
- Net: Even if validation fails, we're at 3A + 1week validation = 4 weeks → then full 3B-3C-3D adds 4 more weeks = 8 total (vs 6 original). Cost: +2 weeks to validate.

**But**: If validation SUCCEEDS, we go 3A + 1week + 3D = 4 weeks total. Saving: 2 weeks.

**EV (expected value) of Option C**: 50% chance saves 2 weeks, 50% chance costs 2 weeks. EV = 0.

So Option C is a **risk-neutral** choice. Worth doing if curiosity/confidence is low.

---

## Architectural Soundness Verdict

### MAJOR FINDINGS

1. **The frontier skip question IS architectural**, not engineering
   - Planner correctly elevated it
   - But validation plan is underspecified (missing correctness checks)

2. **Three articles are NOT fully reconciled**
   - Arrow-for-Timely, Timely Protocol, Differential Dataflow have different emphasis
   - Single-threaded wirelog doesn't need Timely's distributed progress protocol
   - But might still need Differential Dataflow's Z-sets for correctness

3. **Option C (validate first) is architecturally sound**
   - De-risks the project by testing key assumptions
   - Requires upgraded validation plan (add DD oracle comparison)

### REQUIRED CHANGES TO INTEGRATION ANALYSIS

Before proceeding to Critic review:

**Change 1**: Clarify what "frontier skip" actually is
- Performance optimization? (skip redundant iterations)
- Progress tracking? (mark intervals as complete)
- Something else from Timely Protocol L3?

**Change 2**: Upgrade validation tasks (Week 1)
```
Task 3.1: Implement frontier skip (unsigned) - DONE
Task 3.2: Implement frontier skip (signed) - DONE
Task 3.3: ADDED - Run both on full benchmark suite
Task 3.4: ADDED - Compare output tuples vs DD oracle
         (CRITICAL: correctness before speed)
Task 3.5: ADDED - If unsigned version is wrong, stop
         (Means Phase 3B is prerequisite)
```

**Change 3**: Tighten success criteria for Option B
```
Current: "If Phase 3D-only hits 47s: multiplicities are optional"
Better: "If Phase 3D produces correct tuples AND hits 47s: optional"
Even better: "On all 15 workloads (not just DOOP)"
```

**Change 4**: Add failure modes
```
Phase 3D alone → 5m, not 47s
→ Means frontier skip doesn't fully explain gap
→ Implies multiplicities (Phase 3B-C) are essential
→ Triggers Option A (full sequential)
```

---

## Architectural Recommendations

### Recommendation 1: Reframe the Question
Not "Is Möbius inversion the key?"
But "What does single-threaded frontier skip require?"

The answer determines everything:
- Requires multiplicities? → Option A
- Doesn't require? → Option B
- Unclear? → Option C (validate)

### Recommendation 2: Validate Against DD Oracle
Before committing 3 weeks to Phase 3D:
1. Implement unsigned frontier skip
2. Run on DOOP
3. Extract DD oracle output from git history (commit 8f03049)
4. Compare: are tuples identical?
5. If yes, proceed with Option B
6. If no, multiplicities are prerequisite (Option A)

### Recommendation 3: Add Correctness Gate
Even if Option B proceeds, add a "correctness verification" step:
- Every phase produces output
- Tuple-by-tuple comparison vs DD oracle
- If any mismatch → roll back to previous phase
- Don't optimize something broken

---

## Verdict: STRUCTURAL SOUNDNESS ✅

The analysis is **architecturally sound** with these provisos:

1. ✅ Problem is correctly framed (frontier skip independence)
2. ✅ Options are genuinely distinct with real tradeoffs
3. ⚠️ BUT: Validation plan needs DD oracle comparison (correctness-critical)
4. ⚠️ AND: Need clarification on what frontier skip actually means per Timely Protocol L3

**Architect's judgment**: Option C is the right choice, but validation must include correctness verification, not just performance measurement.

**Proceed to Critic review** with these modifications.

---

## Open Questions for Critic

1. Can we afford 1-week validation given the schedule?
2. Is DD oracle extraction reliable (git history)?
3. If Phase 3D fails validation, what's the fallback (immediate Option A)?
4. Should we gate DOOP-only, or validate on all 15 workloads?

