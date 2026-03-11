# wirelog

[![Sanitizers](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=Sanitizers)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)
[![Linux GCC](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=Linux%20GCC)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)
[![Linux Clang](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=Linux%20Clang)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)
[![Linux Embedded](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=Linux%20Embedded)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)
[![macOS](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=macOS)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)
[![Windows MSVC](https://img.shields.io/github/actions/workflow/status/justinjoy/wirelog/ci.yml?branch=main&label=Windows%20MSVC)](https://github.com/justinjoy/wirelog/actions/workflows/ci.yml)

**Embedded-to-Enterprise Datalog Engine**

wirelog is a C11-based Datalog engine designed to work seamlessly across embedded systems and enterprise environments. It uses Differential Dataflow for execution and can optionally be optimized with nanoarrow columnar memory for embedded deployments.

## Features

- **Unified Codebase**: Single implementation for embedded and enterprise
- **Differential Dataflow Integration**: Proven incremental processing via Rust FFI
- **Optimization Pipeline**: Logic Fusion, Join-Project Plan (JPP), Semijoin Information Passing (SIP)
- **Benchmark Suite**: 15 workloads from graph analysis to Java points-to analysis (DOOP, 136 rules)
- **Layered Architecture**: Clean separation of Logic, Execution, and I/O
- **FPGA-Ready**: Designed for future hardware acceleration via Arrow IPC
- **Minimal Dependencies**: C11 + Meson build system

## Status

**Phase 0: Foundation** complete. **Phase 1: Optimization** complete.

| Component | Tests | Status |
|-----------|-------|--------|
| Parser (lexer + parser) | 96 | Complete |
| IR (ir + program) | 61 | Complete |
| Stratification | 20 | Complete |
| DD Plan Translator | 22 | Complete |
| FFI Marshalling | 31 | Complete |
| Optimization (Fusion + JPP + SIP) | 36 | Complete |
| DD Execute (end-to-end) | 18 | Complete |
| CLI Driver | 15 | Complete |
| Symbol Interning | 9 | Complete |
| CSV Input | 17 | Complete |
| **C Total** | **325** | **14 suites, all passing** |
| Rust DD Executor | 85 | Complete |
| **Grand Total** | **410** | **All passing** |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for design details.

## Quick Start

```bash
# Clone repository
git clone https://github.com/justinjoy/wirelog.git
cd wirelog

# Build (requires Meson + C11 compiler)
meson setup builddir
meson compile -C builddir

# Run tests
meson test -C builddir
```

## Usage

```bash
# Build with DD executor (requires Rust toolchain)
meson setup builddir -Ddd=true
meson compile -C builddir

# Run a Datalog program
./builddir/wirelog-cli tc.dl

# With multiple workers
./builddir/wirelog-cli --workers 4 tc.dl
```

Example output for a transitive closure program:
```
tc(1, 2)
tc(1, 3)
tc(2, 3)
```

## Architecture

### End-to-End Pipeline

```
.dl file
    ↓ wl_read_file()
Parser (C11, hand-written RDP)
    ↓
IR → Fusion → JPP → SIP
    ↓
DD Plan → FFI Marshal
    ↓
Differential Dataflow (Rust)
    ↓ result callback
Output
```

- **Embedded**: Single-worker DD, memory-constrained
- **Enterprise**: Multi-worker DD (`--workers N`), distributed processing

### Optimization Passes

| Pass | Description |
|------|-------------|
| **Fusion** | Merge adjacent FILTER+PROJECT into FLATMAP |
| **JPP** | Greedy join reorder for 3+ atom chains to minimize intermediate sizes |
| **SIP** | Insert semijoin pre-filters in join chains to reduce intermediate cardinality |

### Benchmark Suite

15 workloads covering graph analysis, pointer analysis, and program analysis:

| Category | Workloads |
|----------|-----------|
| Graph | TC, Reach, CC, SSSP, SG, Bipartite |
| Pointer Analysis | Andersen, CSPA, CSDA, Dyck-2 |
| Advanced | Galen, Polonius, CRDT, DDISASM, DOOP |

### Phase 3+: Optional Embedded Optimization

```
wirelog (C11 parser/optimizer)
    ├─ Enterprise: Differential Dataflow (unchanged)
    └─ Embedded: nanoarrow executor (optional)
```

For details, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Technology Stack

- **Language**: C11
- **Build**: Meson + Ninja
- **Execution**: Differential Dataflow (Rust, dogs3 v0.19.1)
- **Memory** (future): nanoarrow (optional)
- **Hardware Acceleration**: Arrow IPC for FPGA/GPU offload (future)

## Development Roadmap

| Phase | Status | Deliverable |
|-------|--------|-------------|
| 0: Foundation | ✅ Complete | Parser, IR, DD translator, CLI |
| 1: Optimization | ✅ Complete | Fusion, JPP, SIP, 15 benchmarks |
| 2: Documentation | Planned | Language reference, tutorial, examples, CLI docs |
| 3: nanoarrow | Planned | C11-native executor, DD vs nanoarrow comparison |

## Documentation

- **[User Manual](https://justinjoy.github.io/wirelog/)** - Tutorial, language reference, examples, CLI usage
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - Internal system design (developer reference)
- **[LICENSE.md](LICENSE.md)** - Licensing information

## License

wirelog is available under **dual licensing**:

### 1. Open Source: LGPL-3.0

wirelog is distributed under the GNU Lesser General Public License v3.0 (LGPL-3.0).

This allows you to:
- Use wirelog as a library in proprietary applications
- Modify and distribute modified versions
- Link wirelog with proprietary software

For details, see [LICENSE.md](LICENSE.md).

### 2. Commercial License

For proprietary or commercial use cases that require different terms:

**Contact**: inquiry@cleverplant.com

**Use cases include**:
- Closed-source commercial applications
- OEM licensing
- Custom support agreements
- Enterprise deployment with proprietary extensions

See [LICENSE.md](LICENSE.md) for full details.

## CI Workflow

wirelog uses a two-track CI strategy: strict quality gates on pull requests, and comprehensive non-blocking monitoring on the main branch.

### Pull Request Workflows (Blocking)

Every PR targeting `main` runs through three sequential phases. A failure in an earlier phase stops later phases from running.

```
Phase 1: Lint  (lint-pr.yml)
  editorconfig-check
      |
  clang-format-18 check  [blocks if formatting differs from .clang-format]
      |
  clang-tidy-18 check    [blocks if static analysis warnings or errors found]
      |
Phase 2: Build  (ci-pr.yml)
  Linux / GCC ─┐
  Linux / Clang ┼─ fail-fast: first failure cancels remaining matrix jobs
  macOS / Clang ┘
      |
Phase 3: Sanitizers  (ci-pr.yml)
  Linux / GCC  (ASan + UBSan) ─┐
  Linux / Clang (ASan + UBSan) ┘  fail-fast
```

**Phase 1 — Lint** (`lint-pr.yml`, triggered by `ci-pr.yml`):
- EditorConfig: whitespace, encoding, and line-ending consistency (seconds)
- clang-format 18: formatting must exactly match `.clang-format` style
- clang-tidy 18: static analysis; warnings and errors both block the PR (up to 30 min)

**Phase 2 — Build and Test** (`ci-pr.yml`):
- Platforms: Linux GCC, Linux Clang, macOS Clang
- Runs `meson test` with `--print-errorlogs`; all tests must pass

**Phase 3 — Sanitizers** (`ci-pr.yml`):
- Linux GCC and Clang with `-Db_sanitize=address,undefined`
- `ASAN_OPTIONS=abort_on_error=1:halt_on_error=1`
- `UBSAN_OPTIONS=abort_on_error=1:halt_on_error=1:print_stacktrace=1`

A PR cannot be merged unless all three phases pass.

### Main Branch Workflows (Non-Blocking Monitoring)

Pushes to `main` trigger broader coverage that runs to completion regardless of individual failures. Results appear in GitHub Actions step summaries as informational annotations.

```
All phases run in parallel, continue-on-error: true

Lint monitor        (lint-main.yml)
  editorconfig-check (monitor)
  clang-format-18    (monitor)
  clang-tidy-18      (monitor)

Build monitor       (ci-main.yml)
  Linux / GCC
  Linux / Clang
  macOS / Clang
  Windows / MSVC      <-- additional platform vs PR workflow

Sanitizers monitor  (ci-main.yml)
  Linux / GCC   (ASan + UBSan)
  Linux / Clang (ASan + UBSan)
  macOS / Clang (ASan + UBSan)  <-- additional platform vs PR workflow
```

The main branch monitor adds Windows MSVC builds and macOS sanitizer runs that are too slow to gate PRs. Failures are visible in the Actions tab but do not block subsequent commits.

### Workflow File Reference

| File | Trigger | Mode | Purpose |
|------|---------|------|---------|
| `lint-pr.yml` | PR to `main` (also called by `ci-pr.yml`) | Blocking | Sequential lint gates |
| `ci-pr.yml` | PR to `main` | Blocking | Build + sanitizer gates |
| `lint-main.yml` | Push to `main` | Non-blocking | Lint health monitoring |
| `ci-main.yml` | Push to `main` | Non-blocking | Comprehensive build + sanitizer monitoring |

### Developer Guide

**Before opening a PR**, run these checks locally to avoid CI failures:

```bash
# 1. Format all modified C files (required — CI hard-gates on this)
/opt/homebrew/opt/llvm@18/bin/clang-format --style=file -i wirelog/*.c wirelog/*.h

# 2. Verify no formatting diff remains
git diff wirelog/ tests/

# 3. Run the full test suite
meson test -C build --print-errorlogs

# 4. Optional: run with sanitizers locally
meson setup build-san -Db_sanitize=address,undefined -Db_lundef=false -Dtests=true --buildtype=debug
meson test -C build-san --print-errorlogs
```

**Pre-commit hook (recommended)**: Add this to `.git/hooks/pre-commit` to enforce formatting automatically:

```bash
#!/bin/sh
CLANG_FORMAT=/opt/homebrew/opt/llvm@18/bin/clang-format
changed=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(c|h)$')
if [ -n "$changed" ]; then
    echo "$changed" | xargs "$CLANG_FORMAT" --style=file -i
    echo "$changed" | xargs git add
fi
```

**PR merge requirements:**
1. All three CI phases must be green (lint, build, sanitizers)
2. clang-format 18 must report zero violations
3. clang-tidy 18 must report zero warnings or errors
4. All tests must pass on Linux GCC, Linux Clang, and macOS Clang

**Interpreting main branch CI failures:**
- Failures in `CI Main` and `Lint Main` are informational and do not need to be resolved before the next commit
- They should be investigated and addressed in a follow-up PR
- The Windows MSVC job and macOS sanitizer job exist only in the main monitor; failures there should be triaged as normal bugs

## Contributing

wirelog is open source under LGPL-3.0. Contributions are welcome!

Please review our [Contributing Guidelines](CONTRIBUTING.md), [Code of Conduct](CODE_OF_CONDUCT.md), and [Security Policy](SECURITY.md) prior to submitting pull requests or reporting security issues.

By submitting a contribution, you agree to the [Contributor License Agreement (CLA)](CLA.md). This is required because wirelog uses dual licensing (LGPL-3.0 + Commercial).

## Support

- **Documentation**: [docs/](docs/)
- **Issues & Discussions**: GitHub Issues
- **Commercial Support**: inquiry@cleverplant.com

## References

- **FlowLog Paper**: "[FlowLog: Efficient and Extensible Datalog via Incrementality](https://arxiv.org/pdf/2511.00865)" (PVLDB 2025)
- **Differential Dataflow**: [TimelyDataflow/differential-dataflow](https://github.com/TimelyDataflow/differential-dataflow)
- **Apache Arrow**: [Apache/Arrow](https://arrow.apache.org/)

---

**wirelog** - Building bridges between embedded and enterprise data processing.
