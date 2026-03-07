# Threading/FPGA Trade-off Analysis Matrix

**Project**: wirelog - Embedded-to-Enterprise Datalog Engine
**Date**: 2026-03-06
**Branch**: `next/pure-c11`
**Status**: Phase 2C (Pure C11 Columnar Backend)

---

## 1. Overview

This document provides a quantitative comparison of four threading/execution model options for the wirelog columnar backend. Each option is scored across 8 dimensions on a 1-5 scale (5 = best), with rationale for extreme scores (1s and 5s). Two weighted priority scenarios are evaluated to determine optimal choice under different strategic assumptions.

### Options Under Evaluation

| Label | Option | Summary |
|-------|--------|---------|
| **A** | Pthread work-stealing pool | Intra-operator parallelism via pthreads; no FPGA abstraction |
| **B** | Async work-queue (day one) | Abstract `wl_work_queue_t` with pluggable CPU + FPGA backends from the start |
| **C** | Event-driven reactor | Single-threaded event loop with optional thread pool farming |
| **D** | Phased approach | Pthreads now (B-lite), work-queue wrapper when FPGA materializes (B-full) |

---

## 2. Scoring Dimensions

| # | Dimension | Description |
|---|-----------|-------------|
| 1 | **Implementation Complexity** | How much design and coding effort is required to deliver the initial working version. Higher score = simpler. |
| 2 | **Debug Tooling Availability** | Quality of existing debugger, sanitizer, and profiler support. Higher score = better tooling. |
| 3 | **Embedded RTOS Compatibility** | Ability to degrade gracefully on bare-metal/RTOS targets without mandatory OS dependencies. Higher score = more compatible. |
| 4 | **FPGA Integration Fit** | How naturally the model maps to FPGA DMA/PCIe bulk transfer patterns and Arrow IPC data paths. Higher score = better fit. |
| 5 | **CPU Parallelism ROI** | Expected performance improvement for CPU-bound workloads (JOIN, CONSOLIDATE) relative to implementation effort. Higher score = better ROI. |
| 6 | **Performance Overhead** | Runtime cost of the threading/dispatch mechanism itself (task creation, synchronization, context switching). Higher score = lower overhead. |
| 7 | **Team Learning Curve** | Ease of adoption for a team with zero existing concurrent code in the codebase. Higher score = easier to learn. |
| 8 | **Upgrade Cost to FPGA** | Cost of transitioning from the initial implementation to full FPGA offload support. Higher score = lower upgrade cost. |

---

## 3. Unweighted Scoring Matrix

### 3.1 Complete 4x8 Matrix

| Dimension | A: Pthreads | B: Work-Queue | C: Event-Driven | D: Phased |
|-----------|:-----------:|:-------------:|:----------------:|:---------:|
| 1. Implementation Complexity | 4 | 2 | 3 | 4 |
| 2. Debug Tooling Availability | **5** | 2 | 3 | **5** |
| 3. Embedded RTOS Compatibility | 3 | 3 | 4 | 4 |
| 4. FPGA Integration Fit | **1** | **5** | 2 | 4 |
| 5. CPU Parallelism ROI | 4 | 3 | **1** | 4 |
| 6. Performance Overhead | 4 | 3 | **5** | 4 |
| 7. Team Learning Curve | **5** | 2 | 3 | **5** |
| 8. Upgrade Cost to FPGA | 2 | **5** | **1** | 4 |
| **Unweighted Total** | **28** | **25** | **22** | **34** |

### 3.2 Score Rationale

#### Extreme Scores (1s and 5s)

**Option A (Pthreads)**:
- **Debug Tooling = 5**: ThreadSanitizer, Helgrind, and GDB have first-class pthread support. These are the most mature concurrent debugging tools available in the C ecosystem. wirelog's first-ever concurrent code benefits maximally from proven tooling.
- **FPGA Integration Fit = 1**: Pthreads assume shared-memory CPU execution. FPGA acceleration requires DMA bulk transfers over PCIe with completion callbacks -- a fundamentally different execution model. With pthreads alone, FPGA integration would require an entirely separate code path with no shared abstraction, resulting in two parallel parallelism models that must coexist.
- **Team Learning Curve = 5**: pthreads are the most widely taught and documented threading API in the C ecosystem. Extensive tutorials, textbooks, and Stack Overflow coverage. The team can leverage decades of community knowledge.

**Option B (Work-Queue)**:
- **FPGA Integration Fit = 5**: The abstract work-queue model (`wl_work_queue_t`) maps directly to both CPU thread pools and FPGA DMA transfer queues. Each work item is a self-contained columnar batch (Arrow IPC), making the CPU-to-FPGA transition a backend swap rather than an architectural change.
- **Upgrade Cost to FPGA = 5**: By design, no upgrade is needed -- the FPGA backend is a pluggable implementation of the same `wl_work_queue_t` interface. Adding FPGA support means implementing a new backend, not refactoring existing code.

**Option C (Event-Driven)**:
- **CPU Parallelism ROI = 1**: The event-driven reactor is fundamentally single-threaded. wirelog's primary bottlenecks are compute-bound (sort-merge JOIN, qsort-based CONSOLIDATE across up to 1,487 fixed-point iterations in Polonius). A single-threaded event loop cannot parallelize these operations. Optional thread farming partially mitigates this but adds the complexity of both models without the benefits of either.
- **Performance Overhead = 5**: A single-threaded event loop has near-zero dispatch overhead -- no thread creation, no synchronization primitives, no context switching. For small workloads (<10K rows), this is the lowest-overhead option.
- **Upgrade Cost to FPGA = 1**: An event-driven reactor has no natural mapping to FPGA DMA patterns. Transitioning to FPGA would require replacing the entire execution model, not extending it. The event loop abstraction provides no reusable components for FPGA integration.

**Option D (Phased)**:
- **Debug Tooling = 5**: Phase B-lite uses raw pthreads, inheriting the same ThreadSanitizer/Helgrind/GDB support as Option A. The team gains concurrent debugging experience before Phase B-full introduces the work-queue abstraction.
- **Team Learning Curve = 5**: Phase B-lite starts with pthreads (most familiar API), allowing the team to build concurrent programming expertise incrementally. The work-queue abstraction (Phase B-full) is introduced only after the team has production experience with threaded code.

#### Mid-Range Scores (2-4)

**Option A**:
- **Implementation Complexity = 4**: Pthreads are well-understood, but `g_consolidate_ncols` global state (`columnar_nanoarrow.c:918`) and qsort thread-safety issues require upfront refactoring (Phase A work). Not a 5 because the global state cleanup is non-trivial.
- **Embedded RTOS Compatibility = 3**: POSIX pthreads are available on many RTOS platforms (FreeRTOS+POSIX, Zephyr POSIX), but not universally. Bare-metal targets without POSIX layer require conditional compilation. Single-threaded fallback is possible but must be explicitly designed.
- **CPU Parallelism ROI = 4**: Direct pthread parallelism for JOIN partition and CONSOLIDATE can yield significant speedups on multi-core CPUs, especially for large relations (Polonius: <20GB). Not a 5 because small relations (<10K rows) may see overhead exceed benefit.
- **Performance Overhead = 4**: Raw pthreads have low per-operation overhead (no task descriptor serialization, no queue management). Slightly below 5 due to thread pool management and work-stealing queue synchronization costs.
- **Upgrade Cost to FPGA = 2**: Wrapping an existing pthread pool in a `wl_work_queue_t` abstraction is feasible (estimated 1-2 weeks per the plan), but requires API surface redesign. Not a 1 because the pthread pool becomes the CPU backend of the work-queue -- no throwaway code.

**Option B**:
- **Implementation Complexity = 2**: Requires designing the `wl_work_queue_t` interface (task descriptor, completion mechanism, error propagation) simultaneously with first-ever concurrent code. The abstraction layer adds design surface area that compounds debugging difficulty.
- **Debug Tooling Availability = 2**: The custom `wl_work_queue_t` abstraction has no existing debugger support. ThreadSanitizer can detect data races in the underlying pthread implementation, but abstraction-level bugs (task ordering, completion semantics, error propagation) require custom debugging instrumentation. This is the "first concurrent code is also most abstract" risk.
- **Embedded RTOS Compatibility = 3**: Same POSIX dependency as Option A for the CPU backend. The abstraction layer itself is platform-agnostic, but the synchronous queue drain fallback must be explicitly implemented and tested.
- **CPU Parallelism ROI = 3**: Achieves the same parallelism as Option A but with higher per-task overhead (task descriptor allocation, queue management). The abstraction tax reduces net ROI for CPU-only workloads.
- **Performance Overhead = 3**: Each work item requires task descriptor allocation, queue insertion, and completion callback dispatch. For fine-grained intra-operator parallelism (e.g., partitioned sort-merge), this overhead is measurable relative to raw pthreads.
- **Team Learning Curve = 2**: The team must simultaneously learn threading concepts, design a novel abstraction, and debug concurrent code through that abstraction. This compounds the learning curve significantly for a codebase with zero existing concurrent code.

**Option C**:
- **Implementation Complexity = 3**: An event loop is conceptually simpler than multi-threading (no shared state), but requires either an external dependency (libuv, libev) or a custom implementation. The optional thread pool farming adds back most of the multi-threading complexity.
- **Debug Tooling Availability = 3**: Single-threaded event loops are straightforward to debug with standard tools (GDB, breakpoints). However, if thread farming is added, debugging becomes split between event-loop logic and threaded execution.
- **Embedded RTOS Compatibility = 4**: Single-threaded execution has the lowest OS dependency -- no threading primitives required. Many RTOS platforms support simple event loops natively. Score reduced from 5 because thread farming (needed for CPU utilization) reintroduces threading dependencies.
- **FPGA Integration Fit = 2**: An event loop can technically wait on DMA completion events, but this mapping is awkward -- FPGA work submission doesn't fit the event-driven callback model naturally. The event loop becomes a bottleneck for high-throughput FPGA offload.
- **Team Learning Curve = 3**: Event-driven programming is moderately familiar, but the async callback style can be challenging for batch-oriented Datalog evaluation. Adding thread farming creates a hybrid model that is harder to reason about.

**Option D**:
- **Implementation Complexity = 4**: Phase B-lite is identical to Option A in complexity. Phase B-full adds the work-queue wrapper, but this is deferred until the team has threading experience and FPGA requirements are concrete. Near-term complexity matches Option A.
- **Embedded RTOS Compatibility = 4**: Phase B-lite inherits Option A's RTOS story (POSIX pthreads with fallback). Phase B-full adds synchronous queue drain as an explicit embedded fallback. The phased approach allows testing embedded compatibility at each stage. Scores higher than A because the explicit fallback design in B-full is part of the plan.
- **FPGA Integration Fit = 4**: Not a 5 because the initial phase (B-lite) uses raw pthreads without FPGA abstraction. However, the explicit plan to introduce `wl_work_queue_t` in Phase B-full means FPGA integration is architecturally planned, not retrofitted. The pthread pool becomes the CPU backend of the work-queue with no throwaway code.
- **CPU Parallelism ROI = 4**: Identical to Option A for Phase B-lite (direct pthread parallelism). Phase B-full adds minimal abstraction overhead. The phased approach delivers CPU parallelism value soonest.
- **Performance Overhead = 4**: Phase B-lite has the same low overhead as Option A. Phase B-full adds thin work-queue wrapper overhead, but this is optimizable since the abstraction wraps a mature, profiled pthread pool.
- **Upgrade Cost to FPGA = 4**: The planned Phase B-full wrap (estimated 1-2 weeks) is a deliberate, designed transition rather than an ad-hoc refactor. Scores lower than Option B's 5 because the wrap is still a distinct engineering effort, but higher than Option A's 2 because the wrap is planned and the pthread API is kept clean for this purpose.

---

## 4. Weighted Scoring Scenarios

### 4.1 Scenario 1: FPGA-First Priority

Assumes FPGA acceleration is the primary strategic goal. FPGA-related dimensions are weighted higher.

| Dimension | Weight | A | B | C | D |
|-----------|:------:|:---:|:---:|:---:|:---:|
| 1. Implementation Complexity | 1.0 | 4.0 | 2.0 | 3.0 | 4.0 |
| 2. Debug Tooling Availability | 1.0 | 5.0 | 2.0 | 3.0 | 5.0 |
| 3. Embedded RTOS Compatibility | 1.0 | 3.0 | 3.0 | 4.0 | 4.0 |
| 4. **FPGA Integration Fit** | **2.0** | **2.0** | **10.0** | **4.0** | **8.0** |
| 5. CPU Parallelism ROI | 0.5 | 2.0 | 1.5 | 0.5 | 2.0 |
| 6. Performance Overhead | 0.5 | 2.0 | 1.5 | 2.5 | 2.0 |
| 7. Team Learning Curve | 1.0 | 5.0 | 2.0 | 3.0 | 5.0 |
| 8. **Upgrade Cost to FPGA** | **2.0** | **4.0** | **10.0** | **2.0** | **8.0** |
| **Weighted Total** | | **27.0** | **32.0** | **22.0** | **38.0** |

**FPGA-First Rankings**:

| Rank | Option | Weighted Score |
|------|--------|:--------------:|
| 1 | **D: Phased** | **38.0** |
| 2 | B: Work-Queue | 32.0 |
| 3 | A: Pthreads | 27.0 |
| 4 | C: Event-Driven | 22.0 |

### 4.2 Scenario 2: CPU Parallelism-First Priority

Assumes CPU performance is the immediate priority; FPGA is deprioritized. CPU performance and implementation simplicity dimensions are weighted higher.

| Dimension | Weight | A | B | C | D |
|-----------|:------:|:---:|:---:|:---:|:---:|
| 1. **Implementation Complexity** | **2.0** | **8.0** | **4.0** | **6.0** | **8.0** |
| 2. **Debug Tooling Availability** | **2.0** | **10.0** | **4.0** | **6.0** | **10.0** |
| 3. Embedded RTOS Compatibility | 1.0 | 3.0 | 3.0 | 4.0 | 4.0 |
| 4. FPGA Integration Fit | 0.5 | 0.5 | 2.5 | 1.0 | 2.0 |
| 5. **CPU Parallelism ROI** | **2.0** | **8.0** | **6.0** | **2.0** | **8.0** |
| 6. Performance Overhead | 1.0 | 4.0 | 3.0 | 5.0 | 4.0 |
| 7. **Team Learning Curve** | **2.0** | **10.0** | **4.0** | **6.0** | **10.0** |
| 8. Upgrade Cost to FPGA | 0.5 | 1.0 | 2.5 | 0.5 | 2.0 |
| **Weighted Total** | | **44.5** | **29.0** | **30.5** | **48.0** |

**CPU Parallelism-First Rankings**:

| Rank | Option | Weighted Score |
|------|--------|:--------------:|
| 1 | **D: Phased** | **48.0** |
| 2 | A: Pthreads | 44.5 |
| 3 | C: Event-Driven | 30.5 |
| 4 | B: Work-Queue | 29.0 |

---

## 5. Sensitivity Analysis

### Score Stability

Option D ranks **first in both scenarios** because it inherits the near-term strengths of Option A (proven tooling, low complexity, high CPU ROI) while preserving the long-term strengths of Option B (FPGA upgrade path, no throwaway code).

| Scenario | Winner | Runner-Up | Gap |
|----------|--------|-----------|:---:|
| FPGA-First | D (38.0) | B (32.0) | +6.0 |
| CPU-First | D (48.0) | A (44.5) | +3.5 |

Key observations:
- **Option A closely tracks Option D** in the CPU-First scenario (gap = 3.5), because A and D share the same near-term implementation (pthreads). D's edge comes from its planned FPGA upgrade path and explicit embedded fallback design.
- **Option B gains ground** in the FPGA-First scenario (gap narrows to 6.0), because B's day-one FPGA abstraction is its primary advantage. However, B's poor scores on debug tooling (2) and team learning curve (2) prevent it from overtaking D.
- **Option C is consistently last or near-last**, reflecting its fundamental unsuitability for compute-bound batch Datalog workloads.

### Risk-Adjusted View

| Option | Primary Risk | Impact if Risk Materializes |
|--------|-------------|----------------------------|
| A | FPGA requirements arrive; no abstraction layer exists | 1-2 week refactor to wrap pthread pool (manageable) |
| B | First concurrent code is also most abstract; debugging compounds | Extended debugging cycles, potential architectural rework of abstraction |
| C | CPU utilization ceiling; requires thread farming to be useful | Hybrid event-loop + threads model negates the simplicity advantage |
| D | Discipline required to keep pthread API clean for future wrapping | Minor -- code review gates can enforce API cleanliness |

Option B carries the highest **downside risk**: if the abstraction design is wrong (likely for a team's first concurrent code), reworking both the abstraction and the threading logic simultaneously is significantly more expensive than fixing either in isolation. Option D mitigates this by separating the two concerns temporally.

---

## 6. Option D: Near-Term vs End-State Scores

Option D's phased nature means its scores evolve over time:

| Dimension | Phase B-lite (Near-Term) | Phase B-full/C (End-State) |
|-----------|:------------------------:|:--------------------------:|
| 1. Implementation Complexity | 4 (= Option A) | 3 (adds abstraction layer) |
| 2. Debug Tooling Availability | 5 (= Option A) | 4 (abstraction adds debugging surface) |
| 3. Embedded RTOS Compatibility | 3 (= Option A) | 4 (synchronous queue drain fallback) |
| 4. FPGA Integration Fit | 1 (= Option A) | 5 (= Option B, full work-queue) |
| 5. CPU Parallelism ROI | 4 (= Option A) | 4 (thin wrapper overhead) |
| 6. Performance Overhead | 4 (= Option A) | 3 (work-queue dispatch cost) |
| 7. Team Learning Curve | 5 (= Option A) | 3 (work-queue concepts) |
| 8. Upgrade Cost to FPGA | 2 (= Option A) | 5 (= Option B, already abstracted) |

This decomposition shows that Option D is effectively **Option A in the near-term** and **Option B at end-state**, capturing the best of both without the worst of either.

---

## 7. Recommendation

### Per-Scenario Winners

- **FPGA-First scenario**: **Option D (Phased)** wins with 38.0 points, beating Option B (32.0) by 6 points. While B scores highest on raw FPGA dimensions, D's superior debug tooling, simpler near-term implementation, and lower team learning curve overcome B's FPGA advantage.

- **CPU Parallelism-First scenario**: **Option D (Phased)** wins with 48.0 points, beating Option A (44.5) by 3.5 points. D matches A on all CPU-focused dimensions and adds the planned FPGA upgrade path and explicit embedded fallback.

### Why Option D is Robust Across Scenarios

Option D wins in both scenarios because it is **temporally adaptive** -- it delivers the right architecture at the right time:

1. **Near-term (Phase B-lite)**: Identical to Option A. Proven pthreads, best-in-class debug tooling, lowest team learning curve. Delivers CPU parallelism value soonest.

2. **Transition (Phase B-full)**: Wraps the mature, battle-tested pthread pool in `wl_work_queue_t`. The 1-2 week estimated refactor cost is justified only when FPGA requirements become concrete, avoiding premature abstraction.

3. **End-state (Phase C)**: Achieves the same architecture as Option B (pluggable work-queue with CPU + FPGA backends) via a safer path that avoids introducing novel abstractions simultaneously with first-ever concurrent code.

4. **No throwaway code**: The pthread pool from Phase B-lite becomes the CPU backend of the work-queue in Phase B-full. Every line of Phase B-lite code contributes to the end-state.

5. **Risk isolation**: Threading bugs and abstraction design bugs are addressed in separate phases, making root-cause analysis dramatically simpler.

**Conclusion**: Option D (Phased approach) is the recommended path regardless of whether FPGA or CPU parallelism is prioritized. It is the only option that scores competitively in both scenarios while maintaining the lowest combined risk profile.
