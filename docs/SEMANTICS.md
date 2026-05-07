# wirelog Semantic Model

This document records observable semantic decisions in the wirelog
engine. Each section is an ADR-style entry: a short statement of the
rule, the rationale, and the public-API surfaces that hold it today.

Each entry is tagged with its **Status**:

- **Stable** — a contract the engine honors across a major version.
  Changes go through a deprecation cycle in `docs/MIGRATION.md`.
- **Current** — describes what the engine does today on the path to
  1.0. May evolve before 1.0 GA based on review or implementation
  realities. See `stable-release-plan.md` for the trajectory.

Until 1.0 ships, expect most entries to be **Current**.

---

## Inline `.dl` facts (Status: Current)

### Rule

Inline ground facts written in a `.dl` source (e.g.
`role_permission("wr.system_admin", "wr.policy.write").`) are part of
the EDB. After parsing the program, the engine seeds these facts into
the columnar session as base rows exactly once, on the first
plan/session build:

- via `wl_easy_open` / any `wl_easy_*` lazy entry point: at first
  build, before any host delta callback can be installed.
- via the CLI driver: at the same point in the
  `wl_session_create` → `wl_session_load_facts` →
  `wl_session_load_input_files` sequence.

Snapshots returned by `wl_easy_snapshot()` and IDB rows derived by the
optimizer pipeline therefore observe inline facts on the first call,
without any host action.

### Z-set semantics for host insert / remove

`wl_easy_insert()` and `wl_easy_remove()` are differential operations
on the session's z-set state:

- `wl_easy_insert(R, row)` raises the multiplicity of `row` in `R`
  by `+1`.
- `wl_easy_remove(R, row)` lowers it by `-1`.

If a host inserts a row that is already present from the inline-fact
seed, the row's multiplicity becomes `+2`. A subsequent
`wl_easy_remove()` of the same row leaves multiplicity `+1`; the row
remains observable in snapshots until both copies are retracted.

This matches differential dataflow conventions and is consistent with
`wl_session_*` (internal) and the future `wirelog_session_*` advanced
surface (see `stable-release-plan.md` §3).

### How a host can mirror static facts safely

Two patterns are supported:

1. **Do nothing** — the engine seeds inline facts on its own. The host
   is free to insert *only* the facts it wants to add at runtime.
2. **Pre-check via `wirelog_program_get_facts`** — a host that needs
   to know which inline facts are present can iterate them and skip
   matching rows in its mirror logic. This avoids the +2 multiplicity
   case if the host design demands set semantics.

### What is *not* promised

- The relative *order* of inline-fact rows visible in a snapshot is
  not stable across releases. Hosts must not depend on positional
  ordering.
- The *identity* of intern ids assigned to symbols in inline facts is
  not stable across runs of the same program.
- Multiplicities other than the basic z-set arithmetic above (e.g.
  weighted aggregates) are subject to the engine's
  multiplicity-tracking rules, not promised by this document.

### Forward compatibility (Future work — not yet shipped)

`wirelog/wirelog-advanced.h` and the public `wirelog_session_*` surface
do not exist in the tree yet. They are planned per
`stable-release-plan.md` §3 and, when they land, are intended to share
this exact semantic model: open-time inline-fact seeding, z-set host
insert/remove, optional pre-check via `wirelog_program_get_facts`. The
intent here is to avoid drift between the easy facade and the future
advanced surface; this paragraph is non-binding until those headers
actually ship.

### References

- `wirelog/wl_easy.c` — `ensure_plan_built` performs the seed.
- `wirelog/session_facts.c` — backend-agnostic loader.
- `wirelog/cli/driver.c` — the canonical sequence the easy facade mirrors.
- `wirelog/wirelog.h` — `wirelog_program_get_facts` for host pre-check.
- `CHANGELOG.md` — entry for #718.
- `stable-release-plan.md` §3, §7 — public-surface and conformance plans.
