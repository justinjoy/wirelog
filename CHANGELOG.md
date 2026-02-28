# Changelog

All notable changes to wirelog are documented in this file.

## [0.10.1] - 2026-02-28

### Added

- **Symbol interning** (`wl_intern_t`): bidirectional string-to-integer mapping
  for string-typed columns. The DD executor continues to operate on `Vec<i64>`;
  strings are interned at parse/load time and reverse-mapped on output. (#42)
- **`.input` CSV loading** (`wirelog_load_input_files()`): relations with
  `.input(filename="...", delimiter="...")` directives now load CSV data
  automatically during pipeline execution. (#18)
- **Head arithmetic**: `project_exprs` / `map_exprs` in the DD plan enable
  arithmetic expressions (e.g., `cost(x, c+1)`) in rule heads. (#20)
- **CC and SSSP benchmark workloads**: Connected Components and Single-Source
  Shortest Path programs added to the benchmark suite. (#27)
- **Benchmark framework**: timing utilities, graph data generator, FlowLog
  benchmark driver, and `meson -Dbench=true` build option.
- **CodeQL CI**: GitHub Advanced Security workflow with security-and-quality
  query suite.

### Fixed

- Recursive aggregation (`min`/`max`) not propagating across DD iterations. (#21)
- FFI null-guard checks flagged by CodeQL.
- Version macros in `wirelog.h` now match the project version
  (`WIRELOG_VERSION_MINOR` corrected from 1 to 10).

### Changed

- Rust executor minimized to DD-essential surface only; non-critical Rust code
  removed.
- Architecture docs updated for DD integration and version numbering.

## [0.10.0] - 2026-01-15

Initial Phase 0 release: parser, IR, optimizer, Differential Dataflow executor
via Rust FFI, CLI driver with inline-fact evaluation pipeline.
