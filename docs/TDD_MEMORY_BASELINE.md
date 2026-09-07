# TDD memory baseline and coverage

This document records what the opt-in `WL_MEM_REPORT=1` diagnostic proves. It
does not turn a ledger report into a process-RSS or bounded-memory guarantee.

## Reproduction

The report is emitted at session teardown for the coordinator and each worker
when `WL_MEM_REPORT` is present (the value is not interpreted). Using
`WL_MEM_REPORT=1` is the conventional invocation.
The exact fields ending in `_bytes` are IEC byte counts and are intended for
machine-readable collection. Human-readable fields are retained for operators.

```sh
WL_MEM_REPORT=1 builddir/tests/test_safe_worker_scaling
```

For a workload measurement, preserve the complete stderr, the command, the
build tree and binary hashes, compiler/options, input manifest, worker count,
and effective process/container memory ceiling beside the report. A report
without those identities is an observation, not a reproducible qualification.

## Current evidence

The historical DOOP report attached to issue #1380 records 13,828,835 tuples
and 153 iterations. Its observed process RSS was approximately 52.1 GiB at W1
and 54.1 GiB at W2; the coordinator ledger reported approximately 32.0 GiB
and 15.2 GiB respectively. The original artifact does not recover a complete
candidate tree/binary identity, raw-log location, or all host-limit metadata;
those fields remain **unavailable**, not reconstructed here. It does not prove
unfused W8 completion or bounded DOOP.

## Coverage inventory

| Domain | Current status | Interpretation |
| --- | --- | --- |
| Relation, arena, cache, arrangement, timestamp ledger counters | measured when the owning path calls the ledger | Exact current/peak bytes are reported per ledger; missing call sites are not zero. |
| Coordinator ledger | measured | Session-owned logical accounting, not process RSS. |
| Worker ledgers | measured at worker teardown | Per-worker lifetime ledger; do not sum peaks as a process peak. |
| Parser/read buffers, CSV token growth, interning | partial/uninstrumented | Must be assigned to the ingestion/governor owners before bounded DOOP is claimed. |
| Channels, external allocations, allocator fragmentation | uninstrumented | OS RSS may exceed ledger totals. |
| Process RSS and cgroup/container ceiling | unavailable in this report | Must be collected as separate host evidence. |
| Spill/disk usage and cleanup | unavailable | No bounded spill qualification is implied. |

The five ledger domains are accounting categories, not proof that every
allocation site is covered. `current_bytes` can return to zero while `peak_bytes`
retains the high-water mark; a worker's teardown report is therefore not a
simultaneous process-wide peak.

## Acceptance boundary

The baseline is suitable for #1381 to consume as a truthful partial inventory.
It is not sufficient to close #1380 until the provenance gaps are either
recovered from immutable artifacts or explicitly attached to the enforcing
owner, and until report-on/report-off correctness and measurement overhead are
recorded. Full low-memory DOOP qualification belongs to #1385; integrated
six-workload enforcement belongs to #1386.
