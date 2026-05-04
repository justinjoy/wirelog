# Example 13: Daemon-Style Session Rotation

## Overview

Long-running daemon processes keep ingesting facts after the initial batch.
When those facts use compound terms, production callers need a way to rotate
session-owned state before bounded arenas saturate. This example exercises the
rotation lifecycle with an inline compound declaration and a deterministic
caller-owned watermark.

This example shows the daemon pattern using only the installed public API:

- keep a caller-owned copy of long-lived EDB facts
- stream new facts through `wl_easy_insert`
- consume changes through `wl_easy_set_delta_cb` and `wl_easy_step`
- rotate by closing the old session, opening a new one, and replaying the EDB
- log operator-visible lifecycle data to `stderr`

The executable uses a public inline compound declaration for the event
metadata:

```datalog
.decl event(id: int64, tenant: symbol, payload: metadata/4 inline)
.decl hot_event(id: int64, tenant: symbol)
hot_event(ID, Tenant) :-
    event(ID, Tenant, metadata(Level, Ts, Host, Risk)), Risk > 80.
```

That keeps the daemon lifecycle executable today while documenting the remaining
API gap that #550 is meant to close: saturation-aware rotation.
The EDB log stores source-level event values rather than encoded Wirelog rows,
so each fresh session re-interns symbols and rebuilds rows with its own public
API state.

## Current Public Pattern

The current public API does not expose a compound arena epoch, a saturation
callback, or a session-rotation primitive. Because of that, this example uses a
caller-owned watermark rather than internal headers:

```c
if (events_since_rotation >= rotate_every) {
    wl_easy_step(old);
    wl_easy_close(old);
    wl_easy_open(program, &fresh);
    replay_edb(fresh, &edb);
}
```

Replay into the fresh session is intentionally separated from live delivery:
the demo restores historical EDB facts before reattaching the live delta path,
then keeps a monotonic `hot_event` id watermark in the consumer. That makes the
downstream side effect idempotent even if a public delta step re-presents
already-known rows after rotation. The pattern is deliberately conservative and
demonstrates the lifecycle shape without including internal headers such as
`wirelog/columnar/internal.h`.

## After #550

With #550 Option A, the trigger can become an engine-owned epoch query:

```diff
- if (events_since_rotation >= rotate_every) {
+ if (wl_session_compound_epoch(session) >= 3686) {
      rotate_session(&session, &edb);
  }
```

With #550 Option B, polling can disappear:

```diff
- if (wl_session_compound_epoch(session) >= 3686) {
-     rotate_session(&session, &edb);
- }
+ wl_session_set_compound_saturation_cb(session, on_saturation, &daemon);
```

With #550 Option C, the caller-owned EDB replay disappears:

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
./build/examples/13-daemon-style/daemon_style_demo \
  --events 256 --rotate-every 64 --log-every 64
```

Or use the wrapper:

```sh
examples/13-daemon-style/run.sh
```

The wrapper defaults to a paced 10-second run. Override
`WIRELOG_DAEMON_SECONDS`, `WIRELOG_DAEMON_EVENTS`,
`WIRELOG_DAEMON_ROTATE_EVERY`, `WIRELOG_DAEMON_LOG_EVERY`, or
`WIRELOG_DAEMON_SLEEP_MS` to shorten or lengthen it.

## Sample Trace

The exact RSS value depends on platform and allocator state.

```text
[daemon] t=0.01s events=64 edb=64 hot=12 rotations=0 rss_kb=4096
[daemon] rotation request: events_since_rotation=64 watermark=64
[daemon] t=0.03s events=128 edb=128 hot=25 rotations=1 rss_kb=4224
[daemon] rotation request: events_since_rotation=64 watermark=64
[daemon] t=0.04s events=192 edb=192 hot=38 rotations=2 rss_kb=4288
[daemon] rotation request: events_since_rotation=64 watermark=64
[daemon] t=0.05s events=256 edb=256 hot=51 rotations=3 rss_kb=4352
[daemon] summary: events=256 edb=256 hot=51 rotations=3 rss_kb=4352
```

## What This Demonstrates

- **Daemon lifecycle** -- facts arrive over time, replay is separated from live
  callbacks, and each live `wl_easy_step` publishes only new deltas for that
  epoch.
- **EDB ownership** -- the application owns the durable input log needed to
  reconstruct a fresh session.
- **Rotation observability** -- `stderr` carries event count, EDB size, live
  derived hot-event count, rotation count, and RSS where available.
- **API gap** -- saturation-aware rotation cannot be driven directly from the
  installed public surface today; #550 is the API that should remove that
  workaround.

## See Also

- `docs/COMPOUND_TERMS.md` -- compound term storage and arena lifecycle
- `examples/08-delta-queries/` -- first delta-callback example
- `examples/11-time-evolution/` -- per-epoch delta isolation
- `wirelog/wl_easy.h` -- public convenience facade
