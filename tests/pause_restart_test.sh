#!/bin/bash
# A paused safe POV stays paused across a restart (#961): pause a POV, push the WAL until its Pov
# record is copied forward, SIGKILL and restart a disk-backed orlyi on a loopback volume, and
# require the POV to come back paused with its updates unpromoted. The scenarios are listed at
# the top of tests/pause_restart_test.py.
#
#   ORLY_OUT       build tree (default ../out_orly/debug); needs orlyi, orlyc, orly_dm
#   ORLYI_PREFIX   command to run orlyi and orlyc under ("setarch x86_64 -R" for a TSan build)
#   EXTRA_ARGS     more orlyi flags (one named here replaces the default of the same name)
#
# Needs root for losetup, like restart_test.sh: it re-runs itself under sudo. Ports 19960-19963;
# pass --port=<base> for parallel runs. Prints one summary line starting "PAUSE RESTART:" and
# exits nonzero on any violation.
set -e
cd "$(dirname "$0")/.."
ORLY_OUT="$(cd "${ORLY_OUT:-$PWD/../out_orly/debug}" && pwd)"
for bin in orly/server/orlyi orly/orlyc orly/indy/disk/util/orly_dm; do
  if [ ! -x "$ORLY_OUT/$bin" ]; then
    echo "missing: $ORLY_OUT/$bin (build it first, e.g. make debug)"
    exit 1
  fi
done
if ! python3 -c 'import websocket' 2>/dev/null; then
  echo "pause restart test: python3 can't import websocket; install websocket-client (pip3 install websocket-client)"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  PY_PATH="$(python3 -c 'import os, websocket; print(os.path.dirname(os.path.dirname(websocket.__file__)))')"
  exec sudo --preserve-env=ORLYI_PREFIX,EXTRA_ARGS,TSAN_OPTIONS ORLY_OUT="$ORLY_OUT" \
    PYTHONPATH="$PY_PATH" python3 tests/pause_restart_test.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/pause_restart_test.py "$@"
