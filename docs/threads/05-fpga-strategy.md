# FPGA Acceleration Strategy for wirelog

**Date**: 2026-03-06
**Phase**: 4+ (deferred until prerequisites met)
**Status**: Strategy document -- no implementation until Phase C trigger gates are satisfied

---

## 1. FPGA Acceleration Rationale

### Why FPGA for Datalog?

wirelog's columnar backend (`wirelog/backend/columnar_nanoarrow.c`) stores relations as row-major `int64_t` buffers in `col_rel_t` structs (line 39-48). The core computational bottlenecks -- sort-merge JOINs and CONSOLIDATE deduplication via `qsort` (line 966) -- are regular, data-parallel operations that map well to hardware acceleration.

FPGA acceleration targets two properties of wirelog's workload:

1. **Bulk data parallelism**: JOIN and CONSOLIDATE operate on large columnar arrays with uniform access patterns. FPGAs can pipeline these operations at wire speed.
2. **Fixed-point iteration depth**: Recursive strata iterate until convergence (up to 4,096 iterations per `MAX_ITERATIONS` at line 30). Offloading per-iteration operators to FPGA reduces cumulative latency.

### Why DMA-Based Arrow IPC Over Shared-Memory Parallelism

The current backend is explicitly single-threaded (`columnar_nanoarrow.c:237`: "NOT thread-safe. Each worker thread must own its session."). Two parallelism models are architecturally possible:

**Shared-memory (pthreads)**: Multiple CPU threads access the same `col_rel_t` buffers. This works for CPU-only parallelism but cannot extend to FPGA -- hardware accelerators have separate memory spaces and require explicit data transfer.

**DMA-based Arrow IPC**: Data is serialized into self-describing Arrow IPC batches and transferred via DMA over PCIe to the FPGA. This model:
- Decouples compute from memory ownership (no shared state)
- Uses a standardized wire format (Arrow IPC) that both CPU and FPGA can produce/consume
- Enables double-buffering: overlap DMA transfer with computation
- Aligns with wirelog's existing nanoarrow dependency for columnar storage

Arrow IPC is the preferred FPGA data path because it provides a clean contract between CPU host and FPGA device without requiring shared address space, which FPGA boards do not naturally support.

---

## 2. Arrow IPC Data Transfer Mechanism

### Serialize / Deserialize Flow

```
CPU Host                              FPGA Device
─────────                             ───────────
col_rel_t (row-major int64)
    │
    ▼
Arrow IPC batch formation
  ├─ Schema: ncols × Int64 columns
  ├─ RecordBatch: nrows of data
  └─ Metadata: relation name, key indices
    │
    ▼
DMA write (host → device)            ──► FPGA input buffer
  PCIe Gen3/4 x8                          │
  (8-16 GB/s bandwidth)                   ▼
                                     FPGA kernel execution
                                     (JOIN / CONSOLIDATE)
                                          │
DMA read (device → host)             ◄── FPGA output buffer
    │
    ▼
Arrow IPC deserialization
    │
    ▼
col_rel_t result (row-major int64)
```

### Batch Formation

wirelog relations (`col_rel_t` at `columnar_nanoarrow.c:39-48`) already carry Arrow-compatible metadata:
- `ncols` maps directly to Arrow schema field count
- `data` (row-major `int64_t*`) requires transposition to columnar Arrow layout, or can be sent as a single fixed-size-binary column
- `schema` (`struct ArrowSchema`, line 46) is already present in the struct, initialized lazily (`schema_ok` flag at line 47)

Batch formation converts `col_rel_t` into Arrow IPC `RecordBatch` messages. The existing `nanoarrow` dependency provides serialization primitives.

### DMA vs PCIe Transfer

| Transfer Mode | Mechanism | Latency | Throughput | Use Case |
|--------------|-----------|---------|------------|----------|
| Programmed I/O | CPU writes to MMIO registers | High | Low (MB/s) | Small control messages, kernel parameters |
| DMA (scatter-gather) | Hardware copies from host memory | Low | High (GB/s) | Relation data batches |
| Zero-copy DMA | Pin host pages, FPGA reads directly | Lowest | Highest | Large relations (>1M rows) |

**Recommended**: DMA scatter-gather for all relation transfers. Zero-copy DMA for relations exceeding 1M rows where pinning overhead is amortized.

### Buffer Management

- **Host-side**: Arrow IPC buffers allocated via nanoarrow's buffer API. Pinned memory pool for DMA-eligible buffers.
- **Device-side**: Double-buffered input/output rings. While the FPGA processes batch N, the host DMA-transfers batch N+1.
- **Lifecycle**: Host retains ownership of input buffers until DMA completion callback. Output buffers are device-allocated, DMA'd back, then freed on device after host acknowledgment.

---

## 3. Backend Abstraction Alignment

### `wl_compute_backend_t` Vtable Integration

The FPGA backend implements the existing `wl_compute_backend_t` vtable defined at `wirelog/backend.h:85-107`. **No changes to the vtable are required.** The vtable provides all necessary hooks:

```c
// backend.h:85-107
typedef struct {
    const char *name;                    // "fpga"
    int (*session_create)(...);          // Initialize FPGA context, allocate DMA buffers
    void (*session_destroy)(...);        // Release FPGA resources, unpin memory
    int (*session_insert)(...);          // Stage EDB facts in host buffer
    int (*session_remove)(...);          // Stage EDB removals
    int (*session_step)(...);            // Submit one epoch to FPGA, await completion
    void (*session_set_delta_cb)(...);   // Register delta callback (called on DMA completion)
    int (*session_snapshot)(...);        // Full evaluation: submit all strata to FPGA
} wl_compute_backend_t;
```

**How FPGA maps to each vtable slot:**

| Vtable Slot | Columnar Backend (current) | FPGA Backend (future) |
|---|---|---|
| `session_create` | Allocates `col_rel_t` arrays (`columnar_nanoarrow.c:1470`) | Opens FPGA device, allocates DMA buffer pool, initializes kernel contexts |
| `session_destroy` | Frees relations and delta buffers | Releases DMA buffers, closes FPGA device handle |
| `session_insert` | Appends rows to `col_rel_t.data` | Stages rows in host-side DMA buffer for next transfer |
| `session_snapshot` | Iterates strata on CPU | Serializes relations to Arrow IPC, submits to FPGA via DMA, collects results |
| `session_step` | One epoch of incremental evaluation | Submits one incremental step to FPGA kernel |

The `num_workers` parameter in `session_create` (`backend.h:88`) -- currently ignored (`(void)num_workers` at `columnar_nanoarrow.c:1473`) -- can be repurposed by the FPGA backend to configure the number of parallel FPGA compute units or DMA channels.

### Backend Selection

Backend selection uses the existing pattern: `wl_backend_columnar()` returns the columnar vtable singleton (`backend.h:117-118`). An FPGA backend would provide `wl_backend_fpga()` returning an equivalent singleton. The calling code (`session.c`) dispatches through the vtable pointer without knowing which backend is active.

---

## 4. Operator-Level FPGA Offload Strategy

### FPGA-Offloadable Operators

| Operator | FPGA Suitability | Rationale |
|----------|-----------------|-----------|
| **JOIN** (sort-merge) | **HIGH** | Regular access pattern on sorted `int64_t` arrays. FPGA can pipeline merge-compare at clock speed. Largest single-operator cost in Polonius benchmarks. |
| **CONSOLIDATE** (sort + dedup) | **HIGH** | Currently uses `qsort` with global state (`g_consolidate_ncols` at line 918, `qsort` at line 966). FPGA sorting networks (bitonic/merge-sort) are well-studied and eliminate the global-state problem entirely. |
| **FILTER** (predicate eval) | **MEDIUM** | RPN expression evaluation is branch-heavy on CPU but maps to a fixed pipeline on FPGA. Benefit depends on predicate complexity. |
| **MAP** (projection) | **LOW** | Simple column selection/reordering. Memory-bound on CPU; FPGA adds latency without significant speedup for typical column counts (<10). |
| **REDUCE** (aggregation) | **MEDIUM** | Group-by + aggregate (COUNT, SUM, MIN, MAX). FPGA can pipeline the reduction but benefit depends on group cardinality. |
| **CONCAT** (union) | **LOW** | Buffer concatenation. Memory copy dominated; no compute to offload. |

### CPU-Only Operators (Not Offloadable)

| Operator / Function | Reason |
|---------------------|--------|
| **Fixed-point loop control** | Sequential decision logic (convergence check). Must run on CPU to decide whether to submit next iteration to FPGA. |
| **Symbol interning** (`wl_intern_t`) | String-to-integer mapping with hash table lookups. Irregular access patterns, pointer-chasing. Not suited for FPGA pipelines. |
| **Plan compilation** (IR → execution plan) | One-time setup cost. Tree traversal and allocation. |
| **Delta buffer management** | Control-flow-heavy bookkeeping (which relations changed, callback dispatch). |

### Offload Decision Criteria

An operator should be offloaded to FPGA when:
1. **Data volume exceeds threshold**: Relation size > 10K rows (below this, DMA transfer latency exceeds compute savings)
2. **Operator is compute-bound**: JOIN and CONSOLIDATE on large relations
3. **Iteration count is high**: Recursive strata with many fixed-point iterations amplify per-iteration savings

The FPGA backend should implement a **fallback path**: if a relation is below the size threshold, execute the operator on CPU using the columnar backend's implementation. This avoids DMA overhead for small relations.

---

## 5. Work-Queue Integration

### Phase B-full: `wl_work_queue_t` as the Pluggable Backend Interface

The phased implementation roadmap (see `04-implementation-roadmap.md`) introduces `wl_work_queue_t` in Phase B-full as an abstraction wrapping the pthread pool from Phase B-lite. This same abstraction enables FPGA integration:

```
wl_work_queue_t (abstract interface)
    │
    ├─ CPU backend: pthread pool (Phase B-lite → B-full)
    │   └─ submit(task) → thread picks up → execute on CPU → callback
    │
    └─ FPGA backend: DMA submission queue (Phase C)
        └─ submit(task) → serialize to Arrow IPC → DMA to FPGA → completion interrupt → callback
```

### Work Item Structure

Each work item represents an operator invocation on a relation batch:

```
Work Item:
  ├─ operator_type: JOIN | CONSOLIDATE | FILTER | ...
  ├─ input_batch: Arrow IPC serialized relation(s)
  ├─ parameters: key indices, predicate RPN, etc.
  ├─ completion_cb: function pointer for result delivery
  └─ user_data: opaque context for callback
```

### Queue Semantics

- **Submit**: Enqueue a work item. CPU backend: wake a worker thread. FPGA backend: initiate DMA transfer.
- **Wait**: Block until a specific work item completes. Used for synchronization barriers between strata.
- **Drain**: Process all pending items synchronously. Used for single-threaded fallback on embedded targets without threading or FPGA.

### Why Work-Queue Enables Pluggable Backends

The work-queue decouples *what* to compute (operator + data) from *where* to compute it (CPU thread vs FPGA). The `wl_compute_backend_t` vtable (`backend.h:85-107`) handles session-level dispatch; the work-queue handles operator-level dispatch within a session. Together they provide two levels of pluggability:

1. **Session level** (`wl_compute_backend_t`): Choose which backend engine runs the session
2. **Operator level** (`wl_work_queue_t`): Within a session, route individual operators to CPU or FPGA based on data size and operator type

---

## 6. Hardware Selection Criteria

### Required Board Characteristics

| Criterion | Minimum | Recommended | Rationale |
|-----------|---------|-------------|-----------|
| **PCIe interface** | Gen3 x8 (8 GB/s) | Gen4 x8 (16 GB/s) | Arrow IPC batch transfer bandwidth. JOIN on 1M-row relations produces ~8MB batches; need <1ms transfer time. |
| **On-board memory** | 4 GB DDR4 | 8+ GB HBM | Must hold at least the largest relation pair for JOIN. Polonius benchmark reaches ~20GB total; FPGA processes partitioned subsets. |
| **Logic cells** | 500K LUTs | 1M+ LUTs | Sorting networks (bitonic sort for CONSOLIDATE) and merge-join pipelines require significant logic. |
| **DSP slices** | 1000+ | 2000+ | Useful for aggregation (REDUCE) arithmetic pipelines. |
| **Vendor toolchain** | HLS (C/C++ to HDL) | HLS + OpenCL | HLS allows writing FPGA kernels in C, reducing the gap between wirelog's C11 codebase and FPGA implementation. |

### Vendor Considerations

| Vendor | Boards | HLS Toolchain | Ecosystem |
|--------|--------|---------------|-----------|
| AMD/Xilinx | Alveo U250, U280 | Vitis HLS (C/C++ → HDL) | Mature, extensive IP library |
| Intel/Altera | Agilex, Stratix 10 | Intel HLS, oneAPI | Good PCIe support, oneAPI for heterogeneous compute |
| Lattice | Limited high-end | Radiant | Not recommended (insufficient logic density) |
| Open-source | IceStorm/Yosys | Chisel → Verilog | Research only; insufficient for production workloads |

**Recommendation**: Target AMD/Xilinx Alveo or Intel Agilex boards. Both provide PCIe Gen4, HBM, and mature HLS toolchains. Avoid vendor-specific APIs in the `wl_work_queue_t` FPGA backend -- use PCIe DMA at the lowest level, with vendor-specific details isolated to a thin HAL (Hardware Abstraction Layer).

---

## 7. Feasibility Risks and Mitigations

### Risk 1: Arrow Serialization Overhead May Exceed FPGA Compute Benefit

**Description**: Converting `col_rel_t` row-major data (line 42: `int64_t *data`) to Arrow IPC columnar format incurs CPU time for transposition and serialization. For small relations or simple operators, this overhead may exceed the FPGA compute savings.

**Likelihood**: Medium. The crossover point depends on relation size and operator complexity.

**Mitigation**:
- **Prototype before committing**: Build a standalone Arrow IPC round-trip benchmark (serialize `col_rel_t` → Arrow IPC → DMA simulate → deserialize) measuring latency for relations of 1K, 10K, 100K, 1M rows. Establish the minimum relation size where FPGA offload breaks even.
- **Size-based routing**: The work-queue submits to FPGA only when relation size exceeds the measured crossover threshold; smaller relations stay on CPU.
- **Lazy transposition**: If row-major → columnar transposition is the bottleneck, consider storing large relations in columnar layout natively (aligning with nanoarrow's columnar design).

### Risk 2: FPGA Toolchain Vendor Lock-In

**Description**: FPGA development requires vendor-specific synthesis tools (Vivado for Xilinx, Quartus for Intel). Kernels written for one vendor's HLS may not port cleanly to another.

**Likelihood**: High. HLS toolchains have significant behavioral differences.

**Mitigation**:
- **Target generic PCIe DMA, not proprietary accelerator frameworks**: The FPGA backend communicates via standard PCIe DMA, not vendor frameworks like Xilinx XRT or Intel OPAE. Vendor-specific code is isolated in a thin HAL (<500 LOC) that handles device open/close, DMA buffer allocation, and kernel launch.
- **Write kernels in standard C for HLS**: Both Xilinx Vitis HLS and Intel HLS accept C/C++ input. Keeping kernels in portable C maximizes cross-vendor compatibility.
- **Avoid vendor IP cores for core algorithms**: Implement sort/merge-join in pure HLS C rather than using vendor-specific sorting IP. Use vendor IP only for infrastructure (PCIe endpoint, memory controller).

### Risk 3: No Embedded FPGA Ecosystem for C11 Datalog

**Description**: There is no existing FPGA implementation of Datalog evaluation or semi-naive fixed-point iteration. The wirelog FPGA backend would be a first-of-kind effort with no reference implementations to validate against.

**Likelihood**: High. This is novel engineering.

**Mitigation**:
- **Leverage existing HLS ecosystems**: Sorting networks, merge-join, and hash-join are well-studied FPGA primitives with published HLS implementations. The wirelog FPGA backend composes these primitives rather than implementing Datalog evaluation end-to-end on FPGA.
- **CPU-controlled iteration loop**: The fixed-point convergence check remains on CPU. FPGA accelerates individual operators (JOIN, CONSOLIDATE) within each iteration, not the iteration control flow. This reduces the FPGA problem to accelerating well-understood relational operators.
- **Incremental validation**: Start with a single operator (CONSOLIDATE/sort, which has the simplest interface) on FPGA. Validate correctness against CPU output before adding JOIN.
- **Chisel/SpinalHDL as fallback**: If HLS proves insufficient for complex operators, Chisel (Scala → Verilog) provides a higher-level hardware description language with an active open-source community.

### Risk 4: DMA Buffer Alignment and Memory Pinning Complexity

**Description**: Efficient DMA requires page-aligned, pinned host memory. nanoarrow's default allocator (`malloc`) does not guarantee alignment or pinning.

**Likelihood**: Medium. Solvable but requires allocator integration.

**Mitigation**:
- Implement a DMA-aware allocator for the FPGA backend that allocates page-aligned, pinned memory for relation buffers exceeding the FPGA offload threshold.
- Small relations (below threshold) continue using standard `malloc` and execute on CPU.
- The allocator ADR ([Discussion #58](https://github.com/justinjoy/wirelog/discussions/58)) already plans for pluggable allocators (arena allocator); DMA-aware allocation extends this pattern.

---

## 8. Phase 4+ Roadmap

### When to Revisit FPGA

FPGA work begins only when **all four Phase C trigger gates** are met:

1. **FPGA hardware platform selected and available for testing** -- a physical board is in-hand, not a purchase order
2. **Phase 3 (documentation and embedded optimization) complete** -- nanoarrow executor is documented and optimized for embedded targets
3. **Phase B-lite benchmarks demonstrate CPU parallelism is insufficient** -- Polonius p99 latency is still unacceptable after pthread parallelism is deployed
4. **Arrow IPC serialization feasibility validated via prototype** -- round-trip serialize/deserialize of a representative relation batch demonstrates acceptable overhead

**Estimated timeline**: T+18 months from Phase A (global state elimination) completion. This is not a deadline but a planning horizon; actual start depends on gate satisfaction.

### What to Prototype First

1. **Arrow IPC round-trip benchmark** (pre-gate validation):
   - Serialize a `col_rel_t` with 100K-1M rows to Arrow IPC
   - Measure serialization time, buffer size, deserialization time
   - Compare against per-iteration operator execution time
   - Decision: if serialization > 50% of operator time, defer FPGA until relation sizes grow

2. **CONSOLIDATE on FPGA** (first kernel):
   - Implement bitonic sort network in HLS C for `int64_t` arrays
   - Compare FPGA sort latency vs CPU `qsort` (`columnar_nanoarrow.c:966`)
   - Validate output matches CPU CONSOLIDATE bit-for-bit
   - This operator has the simplest interface (single input/output relation, no key matching)

3. **JOIN on FPGA** (second kernel):
   - Implement merge-join pipeline in HLS C
   - Requires two input relations, produces one output
   - More complex DMA pattern (two input buffers, one output buffer)
   - Benchmark against CPU sort-merge JOIN

### Hardware Candidates

| Board | Interface | Memory | Logic | HLS | Estimated Cost |
|-------|-----------|--------|-------|-----|---------------|
| AMD Alveo U250 | PCIe Gen3 x16 | 64 GB DDR4 | 1.3M LUTs | Vitis HLS | ~$5,000 |
| AMD Alveo U280 | PCIe Gen3 x16 | 8 GB HBM | 1.3M LUTs | Vitis HLS | ~$8,000 |
| Intel Agilex F-Series | PCIe Gen4 x16 | DDR4/HBM | 1.4M ALMs | Intel HLS / oneAPI | ~$7,000 |
| BittWare IA-840F | PCIe Gen4 x16 | 32 GB HBM2e | Agilex | Intel HLS | ~$10,000 |

**Starting recommendation**: AMD Alveo U250 for prototyping (best HLS toolchain maturity, sufficient memory for Polonius-scale benchmarks, lowest cost entry point).

---

## 9. References

- `wirelog/backend.h:85-107` -- `wl_compute_backend_t` vtable definition (FPGA integration point)
- `wirelog/backend.h:88` -- `num_workers` parameter in `session_create` (repurposable for FPGA compute units)
- `wirelog/backend.h:117-118` -- `wl_backend_columnar()` singleton pattern (FPGA backend follows same pattern)
- `wirelog/backend/columnar_nanoarrow.c:39-48` -- `col_rel_t` struct (data format for Arrow IPC serialization)
- `wirelog/backend/columnar_nanoarrow.c:237` -- Thread safety comment ("NOT thread-safe")
- `wirelog/backend/columnar_nanoarrow.c:918` -- `g_consolidate_ncols` global state (eliminated by FPGA sort network)
- `wirelog/backend/columnar_nanoarrow.c:966` -- `qsort` call in CONSOLIDATE (FPGA replacement target)
- `wirelog/backend/columnar_nanoarrow.c:1470-1473` -- `col_session_create` ignoring `num_workers`
- `ARCHITECTURE.md:54-57` -- FPGA path in future backend selection diagram
- `ARCHITECTURE.md:128-146` -- Future FPGA path description with Arrow IPC + ComputeBackend abstraction
- `ARCHITECTURE.md:375-383` -- Phase 4 roadmap (FPGA support goals)
