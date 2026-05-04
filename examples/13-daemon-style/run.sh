#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${BUILD_DIR:-"$root_dir/build"}
demo="$build_dir/examples/13-daemon-style/daemon_style_demo"

if [ ! -x "$demo" ]; then
  echo "daemon_style_demo is not built; run: meson compile -C $build_dir daemon_style_demo" >&2
  exit 1
fi

"$demo" --events "${WIRELOG_DAEMON_EVENTS:-1000}" \
  --seconds "${WIRELOG_DAEMON_SECONDS:-10}" \
  --rotate-every "${WIRELOG_DAEMON_ROTATE_EVERY:-128}" \
  --log-every "${WIRELOG_DAEMON_LOG_EVERY:-64}" \
  --sleep-ms "${WIRELOG_DAEMON_SLEEP_MS:-10}"
