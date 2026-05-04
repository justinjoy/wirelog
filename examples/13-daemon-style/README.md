# Example 13: Daemon-Style Session Rotation

## Overview

Long-running daemon processes keep ingesting facts after the initial batch.
When those facts use compound terms, production callers need to rotate
session-owned state after bounded arenas saturate. This example exercises the
rotation lifecycle with a side compound declaration and the public
`wirelog_easy_make_compound` saturation signal.

This example shows the daemon pattern using only the installed public API:

- keep a caller-owned copy of long-lived EDB facts
- stream new facts through `wl_easy_insert`
- consume changes through `wl_easy_set_delta_cb` and `wl_easy_step`
- rotate by closing the old session, opening a new one, and replaying the EDB
- log operator-visible lifecycle data to `stderr`

The executable uses a public side compound declaration for event metadata:

```datalog
.decl event(id: int64, tenant: symbol, risk: int64, payload: metadata/4 side)
.decl hot_event(id: int64, tenant: symbol)
hot_event(ID, Tenant) :- event(ID, Tenant, Risk, _), Risk > 80.
```

The `risk` scalar drives the rule because side-compound body destructuring is a
larger compiler feature tracked separately. The payload still uses the real
session-local compound arena, so rotations are triggered by arena saturation
rather than by a caller-owned event watermark.
The EDB log stores source-level event values rather than encoded Wirelog rows,
so each fresh session re-interns symbols and rebuilds rows with its own public
API state.

## Saturation Pattern

The public API returns `WIRELOG_ERR_COMPOUND_SATURATED` when a new side
compound can no longer be allocated. The daemon handles that by replaying the
durable EDB source log into a fresh session and retrying the current event:

```c
rc = wirelog_easy_make_compound(session, "metadata", 4, args, &payload);
if (rc == WIRELOG_ERR_COMPOUND_SATURATED) {
    rotate_session(&session, &edb);
    rc = wirelog_easy_make_compound(session, "metadata", 4, args, &payload);
}
```

Replay into the fresh session is intentionally separated from live delivery:
the demo restores historical EDB facts before reattaching the live delta path,
then keeps a monotonic `hot_event` id watermark in the consumer. That makes the
downstream side effect idempotent even if a public delta step re-presents
already-known rows after rotation. The pattern is deliberately conservative and
demonstrates the lifecycle shape without including internal headers such as
`wirelog/columnar/internal.h`.

## Remaining Engine Work

The example still keeps EDB ownership in application code. A future
engine-owned rotation primitive could replace the manual close/open/replay
sequence:

```diff
- wl_easy_close(old);
- wl_easy_open(program, &fresh);
- replay_edb(fresh, &edb);
+ wl_session_rotate(session);
```

## Build & Run

```sh
meson compile -C build daemon_style_demo
./build/examples/13-daemon-style/daemon_style_demo
```

For a short deterministic smoke run:

```sh
WIRELOG_COMPOUND_MAX_EPOCHS=64 \
./build/examples/13-daemon-style/daemon_style_demo \
  --events 256 --log-every 64
```

Or use the wrapper:

```sh
examples/13-daemon-style/run.sh
```

The wrapper defaults to a paced 10-second run. Override
`WIRELOG_DAEMON_SECONDS`, `WIRELOG_DAEMON_EVENTS`,
`WIRELOG_DAEMON_LOG_EVERY`, `WIRELOG_DAEMON_SLEEP_MS`, or
`WIRELOG_COMPOUND_MAX_EPOCHS` to shorten or lengthen it.

## Sample Trace

The exact RSS value depends on platform and allocator state.

```text
[daemon] t=0.01s events=64 edb=64 hot=12 rotations=0 saturations=0 rss_kb=4096
[daemon] rotation request: compound arena saturated event=65
[daemon] t=0.03s events=128 edb=128 hot=25 rotations=1 saturations=1 rss_kb=4224
[daemon] rotation request: compound arena saturated event=129
[daemon] t=0.04s events=192 edb=192 hot=38 rotations=2 saturations=2 rss_kb=4288
[daemon] rotation request: compound arena saturated event=193
[daemon] t=0.05s events=256 edb=256 hot=51 rotations=3 saturations=3 rss_kb=4352
[daemon] summary: events=256 edb=256 hot=51 rotations=3 saturations=3 rss_kb=4352
```

## What This Demonstrates

- **Daemon lifecycle** -- facts arrive over time, replay is separated from live
  callbacks, and each live `wl_easy_step` publishes only new deltas for that
  epoch.
- **EDB ownership** -- the application owns the durable input log needed to
  reconstruct a fresh session.
- **Rotation observability** -- `stderr` carries event count, EDB size, live
  derived hot-event count, saturation count, rotation count, and RSS where
  available.
- **Compound-arena pressure** -- tests can set `WIRELOG_COMPOUND_MAX_EPOCHS`
  to force fast deterministic saturation without internal headers.

## See Also

- `docs/COMPOUND_TERMS.md` -- compound term storage and arena lifecycle
- `examples/08-delta-queries/` -- first delta-callback example
- `examples/11-time-evolution/` -- per-epoch delta isolation
- `wirelog/wl_easy.h` -- public convenience facade
