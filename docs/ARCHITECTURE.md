# wirelog Architecture

**Project**: wirelog — Datalog query engine for recursive queries
**Language**: C11 (Phase 1), adding columnar C backend (Phase 2A)
**License**: LGPL-3.0 + Commercial dual license
**Version**: 0.11.0 (Phase 1 Entry)

---

## Vision

wirelog brings Datalog to production applications through a lightweight, modular engine that evolves from Phase 1 (proof-of-concept via Differential Dataflow) to Phase 2 (self-contained C columnar backend) to Phase 3 (distributed multi-worker execution with nanoarrow interchange).

**Phase Roadmap**:
- **Phase 0** ✅ (v0.10.0): Parser, IR, optimizer, DD executor via Rust FFI
- **Phase 1** ✅ (v0.11.0): String interning, CSV loading, head arithmetic, recursive aggregation (monotone: MIN/MAX)
- **Phase 2A** 🚀 (in progress): Complete columnar backend in C11; Rust removal; full recursive delta support
- **Phase 2B**: Multi-operation sessions with incremental updates
- **Phase 3**: Distributed execution, nanoarrow backend, FPGA acceleration, performance optimization

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    CLI / Application Layer                  │
│                  (wirelog-cli, benchmarks)                  │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                   Parser & IR Layer                         │
│  (Lexer → Parser → AST → Rule IR → Stratification)         │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                    Optimizer Layer                          │
│  (Join push-down, predicate fusion, semi-naive planning)   │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                   DD Plan Marshaling                        │
│           (exec_plan.h type definitions + marshal)         │
└────────────────────────┬────────────────────────────────────┘
                         │
           ┌─────────────┴─────────────┐
           │                           │
    ┌──────▼──────┐          ┌────────▼───────┐
    │    Phase 1  │          │     Phase 2A   │
    │ (Rust DD)  │          │  (C Columnar)  │
    │  Executor   │          │   Executor     │
    └──────┬──────┘          └────────┬───────┘
           │                          │
           └──────────┬───────────────┘
                      │
           ┌──────────▼──────────┐
           │    Session Layer    │
           │  (state, snapshot)  │
           └─────────────────────┘
```

### Layer Responsibilities

#### 1. **Parser & IR Layer** (`wirelog/parser/`, `wirelog/ir/`)
- **Input**: Datalog source code (rules + facts)
- **Output**: Stratified rule IR with dependency annotations
- **Components**:
  - `lexer.h`: Tokenization
  - `parser/ast.h`: Abstract syntax tree
  - `ir/program.h`: Rule IR (condition trees for rule bodies)
  - `ir/stratify.h`: Strongly-connected component detection, stratum assignment
- **Key Innovation**: Stratification + SCC detection enable safe negation

#### 2. **Optimizer Layer** (`wirelog/ffi/dd_plan.c`)
- **Input**: Stratified rules
- **Output**: Concrete query plan (`wl_ffi_plan_t`)
- **Operations**:
  - Join reordering (push predicates down first)
  - Operator fusion (combine SCAN+FILTER+MAP)
  - Semi-naive evaluation planning (delta relations for recursion)
  - Aggregation validation (reject non-monotone in recursive strata)
- **Phase 1 Validation**: Rejects COUNT/SUM/AVG in recursive strata (Issue #69)

#### 3. **Plan Layer** (`wirelog/exec_plan.h` + marshaling)
- **Input**: Query plan from optimizer
- **Output**: Marshaled binary representation
- **Type Definitions**:
  - `wl_ffi_plan_t`: Top-level plan (strata, relations, rules)
  - `wl_ffi_op_t`: Operators (SCAN, FILTER, MAP, JOIN, ANTIJOIN, AGGREGATE, UNION)
  - `wl_ffi_expr_buffer_t`: RPN expression buffers for predicates and projections
- **Marshaling**: Convert C structures to binary for cross-language boundaries

#### 4. **Backend Layer** (`wl_compute_backend_t` vtable)
- **Responsibility**: Execute query plan, manage relations, produce output
- **Interface**:
  ```c
  typedef struct {
      int (*create_worker)(const wl_ffi_plan_t *plan, void **worker);
      int (*execute)(void *worker, wl_dd_execute_cb callback, void *user_data);
      int (*free_worker)(void **worker);
  } wl_compute_backend_t;
  ```
- **Phase 1 Implementation**: Differential Dataflow (via Rust FFI)
- **Phase 2A Implementation**: C11 semi-naive evaluator (columnar backend)

#### 5. **Session Layer** (`wirelog/session.h`)
- **Responsibility**: Multi-operation state management
- **Operations**:
  - `session_load_edb`: Load base facts
  - `session_step`: Execute strata until fixed-point
  - `session_insert`: Add new facts
  - `session_snapshot`: Extract results at any stratum boundary
- **Phase 2B Focus**: Incremental updates (insert → step → insert → step)

---

## Phase 1 Architecture (Current)

### Components

| Component | File(s) | Purpose |
|-----------|---------|---------|
| **Parser** | `parser/lexer.c`, `parser/parser.c` | Tokenize + parse Datalog |
| **AST** | `parser/ast.h`, `parser/ast.c` | Syntax tree representation |
| **Rule IR** | `ir/program.c`, `ir/codegen.c` | Convert AST → rule IR (condition trees) |
| **Stratification** | `ir/stratify.c` | SCC detection + stratum assignment |
| **IR Optimizer** | `ffi/dd_plan.c` | Query planning, operator selection |
| **Symbol Interning** | `intern.c` | String ↔ i64 bidirectional mapping (Phase 1 feature) |
| **CSV Loading** | `io/csv.c` | Load `.input(filename=...)` facts (Phase 1 feature) |
| **Rust DD Executor** | `rust/wirelog-dd/src/lib.rs` | Differential Dataflow backend (to be removed Phase 2A) |
| **FFI Bridge** | `ffi/dd_ffi.c`, `ffi/dd_marshal.c` | C ↔ Rust boundary + plan marshaling |
| **CLI** | `cli/main.c` | Command-line interface |

### Data Flow (Phase 1)

```
Datalog source
    ↓
[Lexer]           → tokens
    ↓
[Parser]          → AST
    ↓
[Rule Codegen]    → rule IR (condition trees)
    ↓
[Stratification]  → strata + SCC info
    ↓
[IR Optimizer]    → query plan (wl_ffi_plan_t)
    ↓
[Marshaling]      → binary plan
    ↓
[Rust DD Backend] → relations (Vec<i64>)
    ↓
[Symbol Deinterning] → tuples with strings
    ↓
[Output]          → result tuples
```

### Key Design Decisions (Phase 1)

1. **Rust FFI for Executor**: Leverages mature Differential Dataflow library; provides correctness proof-of-concept. Planned for removal in Phase 2A.

2. **Symbol Interning**: Strings are mapped to sequential i64 IDs at parse/load time. DD operates only on integers. Output deinters on retrieval. Enables string-typed columns without modifying Rust internals.

3. **Semi-Naive Planning**: Optimizer generates delta relations for recursive rules. Prevents redundant re-computation.

4. **Stratified Execution**: Negation only allowed across strata (enforced during planning). Makes negation semantics unambiguous.

5. **Monotone Aggregation Only (Recursive)**: MIN/MAX are safe to use in recursive rules (they are monotone lattice operations). COUNT/SUM/AVG rejected at plan time (Issue #69 validation).

---

## Phase 2A: Columnar C Backend

### Rationale

- **Eliminate Rust Dependency**: 5,045 LOC of Rust code removed; project becomes pure C11.
- **Faster Builds**: Meson C11 compile (<5 sec) vs Cargo Rust (20+ sec).
- **Embedded Target Ready**: C backend easier to port to embedded, FPGA, other platforms.
- **Phase 3 Foundation**: Columnar format enables nanoarrow interchange and distributed execution.

### Architecture Changes

#### New Layer: `wl_compute_backend_t` Vtable
Currently monolithic; Phase 2A introduces pluggable backends:

```c
// Phase 1: DD backend (via Rust)
extern const wl_compute_backend_t wl_backend_dd;

// Phase 2A: Columnar backend (via C)
extern const wl_compute_backend_t wl_backend_columnar;
```

#### New Files (Phase 2A)

| File | Purpose |
|------|---------|
| `wirelog/backend.h` | Backend vtable definition |
| `wirelog/exec_plan.h` | Backend-agnostic plan types (moved from dd_ffi.h) |
| `wirelog/backend_columnar.h` | Columnar backend public API |
| `backend/columnar.c` | Evaluator loop, semi-naive execution |
| `backend/operators.c` | SCAN, FILTER, MAP, JOIN, ANTIJOIN, REDUCE, UNION, SEMIJOIN |
| `backend/memory.c` | Arena allocator, relation store |
| `backend/session_columnar.c` | Session state management for columnar backend |
| `backend/hash_join.c` | Hash join implementation |
| `ffi/exec_marshal.c` | Refactored marshaling (renamed from dd_marshal.c) |

#### Deleted Files (Phase 2A)

- `rust/wirelog-dd/` (entire directory, 5,045 LOC)
- `ffi/dd_ffi.c` (Rust FFI stubs, 85 LOC)
- `backend/backend_dd.c` (DD wrapper, 168 LOC)
- Rust-specific Cargo.toml, Meson rules

#### Modified Files (Phase 2A)

- `backend.h`: Add vtable
- `dd_ffi.h` → `exec_plan.h`: Extract plan types; delete Rust declarations
- `session.h`: Implement backends

### Columnar Backend Algorithm

#### Semi-Naive Evaluation
```
Input: stratified rules + EDB facts
Output: all IDB tuples at fixed-point

for each stratum s:
    initialize: R_s = EDB
    repeat:
        for each rule r in s:
            delta_r = compute_rule(r, previous delta relations)
            R_s.update(delta_r)
        if no deltas produced:
            break  // convergence
    output R_s

delete all deltas (arena cleanup)
```

#### Operators

| Op | Input | Output | Phase 2A Status |
|----|-------|--------|----------------|
| SCAN | base facts | tuples | Unit tested |
| FILTER | tuples + predicate (RPN) | filtered tuples | Unit tested |
| MAP | tuples + projection (RPN) | projected tuples | Unit tested |
| JOIN | relations + join predicate | joined tuples | Unit tested |
| ANTIJOIN | relation + negated relation | difference | Unit tested |
| AGGREGATE | tuples + agg function | grouped aggregates | Non-rec: Phase 1; Rec (MIN/MAX): Phase 2A |
| UNION | relations | concatenation | Deduplication in next iteration |
| SEMIJOIN | relations + join predicate | left relation w/ matched rows | Phase 2A |

#### Memory Management
- **Arena per Iteration**: All delta allocations in one arena; free after convergence
- **Relation Store**: Dynamic array of rows (8-byte i64 columns)
- **Hash Table**: For join/group-by operations
- **Deduplication**: After each iteration, consolidate duplicate rows

### Timeline: 35 Days

| Phase | Days | Activity | Deliverable |
|-------|------|----------|-------------|
| Design | 0-1 | Header structure, memory model, operator interface | Design docs |
| Core | 2-10 | Evaluator loop, SCAN/FILTER/MAP, arena allocator | Simple TC working |
| Recursion | 11-16 | Semi-naive delta, JOIN, ANTIJOIN, multi-relation recursion | TC + Reach pass oracle |
| Features | 17-22 | Sessions, snapshots, remaining operators | CC, SSSP, SG pass |
| Validation | 23-30 | Oracle testing (all 15 benchmarks), ASan cleanup | All benchmarks pass |
| Rust Removal | 31-35 | Delete Rust code, build system, final tests | Ship Phase 2A |

### Phase 2A Success Criteria

**MUST HAVE**:
- ✅ All 15 benchmarks identical to DD (oracle validation)
- ✅ Zero Rust LOC remaining
- ✅ Meson-only build <5 sec
- ✅ Zero ASan/Valgrind errors
- ✅ Execution time <3x DD baseline

**SHOULD HAVE**:
- ✅ All benchmarks including Polonius (1,487 iterations) + DOOP (136 rules, 8-way joins)
- ✅ Multi-operation sessions
- ✅ Snapshot support at stratum boundaries

### Checkpoint: Day 25

**If passing**:
- Proceed to Days 26-35 for final validation + Rust removal
- Full Phase 2A with all 15 benchmarks

**If failing recursion**:
- Release Phase 2A with non-recursive backend
- Defer recursive support to Phase 2B (10 additional days)
- Still achieve Rust removal goal

### References

- **Decision Doc**: `docs/delta-query.md` (RALPLAN consensus)
- **Execution Plan**: `docs/PHASE-2A-EXECUTION.md` (detailed roadmap)
- **Issue #80**: Full Recursive Delta Support specification

---

## Phase 2B & 3: Future Roadmap

### Phase 2B (Post Phase 2A)

**Scope**: Multi-operation sessions with incremental updates
- `session_insert(facts)` → `session_step()` → `session_insert(facts)` → ...
- Incremental delta propagation (don't recompute from scratch)
- Snapshot capture at each stratum boundary
- Session persistence (save/restore state)

**Timeline**: ~2 weeks (builds on Phase 2A columnar backend)

### Phase 3 (Vision)

**Components**:
1. **Multi-Worker Execution**: Shard relations; parallel evaluation on multiple cores
2. **nanoarrow Backend**: Columnar Apache Arrow format for data interchange
3. **FPGA Acceleration**: GPU-friendly operator implementations (join, sort, aggregate)
4. **Performance Optimization**: Target <1x DD baseline execution time
5. **Embedded Targets**: Port to WebAssembly, ARM, etc.

**Strategic Value**: Enable wirelog as infrastructure layer for DOOP, Polonius, network policy analysis at scale.

---

## Technology Choices

### Language: C11
- **Why**: Portability, performance, minimal dependencies
- **Trade-off**: Manual memory management (mitigated by arena allocator + ASan testing)
- **Phase 1 Exception**: Rust for DD (proven library); Phase 2A returns to pure C11

### Build System: Meson
- **Why**: Fast, cross-platform, good C support
- **Alternatives Considered**: CMake (chose Meson for speed), Autotools (chose Meson for simplicity)

### Parser: Hand-Written Lexer/Parser
- **Why**: Datalog syntax is simple; parser is <500 LOC
- **Alternative**: Generated (lex/yacc) — chose hand-written for control

### Testing Strategy
- **Unit Tests**: Per-component (lexer, parser, IR, optimizer, operators)
- **Integration Tests**: End-to-end via CLI (parse Datalog → execute → verify output)
- **Oracle Tests**: Hand-computed expected outputs (Phase 2A innovation)
- **Benchmark Suites**: TC, Reach, CC, SSSP, SG, Bipartite, Polonius, DOOP, etc.

### Continuous Integration
- **GitHub Actions**: Compile + test on Linux/macOS/Windows
- **CodeQL**: Security + code quality analysis
- **Benchmarking**: Track performance regression across commits

---

## Benchmarking

### Workloads (15 Total)

| Benchmark | Rules | Iterations | Phase 1 | Phase 2A |
|-----------|-------|-----------|---------|----------|
| TC (4-node path) | 2 | 3 | ✅ | ✅ |
| Reach (reachability) | 2 | 2 | ✅ | ✅ |
| CC (connected components) | 3 | 4 | ✅ | ✅ (w/ MIN) |
| SSSP (shortest path) | 3 | ∞ | ✅ | ✅ (w/ MAX) |
| SG (same generation) | 4 | 3 | ✅ | ✅ |
| Bipartite (coloring) | 2 | 2 | ✅ | ✅ |
| Polonius (borrow checker) | 37 | 1,487 | ✅ | 🚀 (Phase 2A critical path) |
| DOOP (Java points-to) | 136 | ∞ | ✅ | 🚀 (8-way joins) |
| FlowLog (network policy) | 12 | varies | ✅ | (Phase 2B+) |
| (6 reserved) | | | | |

### Performance Goals

- **Phase 1**: Baseline (DD executor)
- **Phase 2A**: ≤3x DD (acceptable for feature parity)
- **Phase 3**: <1x DD (performance-optimized)

---

## Code Organization

```
wirelog/
├── docs/
│   ├── ARCHITECTURE.md          ← You are here
│   ├── delta-query.md           ← Phase 2A decision
│   ├── PHASE-2A-EXECUTION.md    ← Execution roadmap
│   ├── MONOTONE_AGGREGATION.md  ← Phase 1 innovation
│   └── README.md
├── parser/
│   ├── lexer.c, lexer.h
│   ├── parser.c, parser.h
│   └── ast.c, ast.h
├── ir/
│   ├── program.c, program.h
│   ├── codegen.c, codegen.h
│   ├── stratify.c, stratify.h
│   └── optimizer.c, optimizer.h
├── ffi/
│   ├── dd_plan.c, dd_plan.h     ← IR optimizer
│   ├── exec_plan.h              ← Phase 2A: plan types (new)
│   ├── exec_marshal.c           ← Phase 2A: refactored from dd_marshal.c
│   ├── dd_ffi.c, dd_ffi.h       ← Phase 1: DD boundary (Phase 2A: split + delete)
│   └── backend.h                ← Phase 2A: vtable (new)
├── backend/
│   ├── backend_dd.c             ← Phase 1: DD wrapper
│   ├── backend_columnar.c       ← Phase 2A: columnar evaluator (new)
│   ├── operators.c              ← Phase 2A: operator implementations (new)
│   ├── memory.c                 ← Phase 2A: arena allocator (new)
│   ├── hash_join.c              ← Phase 2A: join implementation (new)
│   └── session_columnar.c       ← Phase 2A: columnar sessions (new)
├── io/
│   ├── csv.c, csv.h             ← Phase 1: CSV loading
│   └── intern.c, intern.h       ← Phase 1: symbol interning
├── session.h                    ← Session API
├── wirelog.h                    ← Public API
├── wirelog-types.h              ← Public types
├── wirelog-ir.h                 ← IR public API
├── wirelog-parser.h             ← Parser public API
├── wirelog-optimizer.h          ← Optimizer public API
├── backend.h                    ← Internal: backend vtable
├── CLAUDE.md                    ← Project instructions
├── AGENTS.md                    ← Agent guidelines
└── meson.build

rust/
└── wirelog-dd/                  ← Phase 1 only; DELETED in Phase 2A

tests/
├── test_*.c                     ← ~20 unit test files
└── meson.build                  ← Test registration

bench/
├── bench_flowlog.c              ← Benchmark driver
├── data/
│   └── graph_*.csv              ← Test data
└── meson.build

build/                           ← Meson output (not in git)
```

---

## Contributions & Development

### Adding a New Operator (Phase 2A Example)

1. Define operator type in `wirelog/ir/...` if new IR node
2. Add plan type in `ffi/dd_plan.c` (wl_ffi_op_t case)
3. Implement in `backend/operators.c` + header
4. Write unit test in `tests/test_operators_*.c`
5. Test integration via benchmark

### Running Tests

```bash
# Build + test
meson setup build
meson compile -C build
meson test -C build

# Run specific test
./build/<test_name>

# Benchmark
./build/bench/bench_flowlog --workload all --data bench/data/graph_10.csv
```

### Code Style

- **C11 compliance**: `-std=c11 -Wpedantic`
- **Linting**: `clang-format --style=file` (version 18)
- **Naming**: Public API `wirelog_*`, internal `wl_*`, subdirectories encode path (`wl_ir_stratify_*`)
- **Testing**: Each test file standalone executable with `TEST/PASS/FAIL` macros

---

## References

- **CLAUDE.md**: Project configuration
- **AGENTS.md**: Agent development guidelines
- **delta-query.md**: Phase 2A RALPLAN consensus decision
- **PHASE-2A-EXECUTION.md**: Detailed 35-day execution roadmap
- **MONOTONE_AGGREGATION.md**: Phase 1 recursion innovation (MIN/MAX semantics)
- **GitHub Issues**: #69 (completed), #80 (Phase 2A focus)

---

**Last Updated**: 2026-03-04
**Status**: Phase 1 complete; Phase 2A ready for execution
