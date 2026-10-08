#!/bin/bash
# Graceful stop under write load (#744): SIGTERM a disk-backed orlyi while eight writers write
# to it, require it to stop on its own within a deadline, restart it on the same loopback volume,
# and require every acknowledged write back. The checks are listed at the top of
# tests/graceful_stop_test.py.
#
#   STOPS=4        graceful stops per run (default 4)
#   DEADLINE=360   seconds each stop may take (default 360: orlyi waits for Tetris while it
#                  promotes, up to its 300 s --stop_promote_budget_s, #769)
#   LONG_STOPS=1   how many of the stops (the last ones) are long-load stops: the load runs until
#                  the POVs' backlogs are 75% full or LONG_MAX_RUN=30 s pass (#769)
#   SEED=<n>       random seed for the load lengths (default: the clock; printed)
#   ORLY_OUT       build tree (default ../out_orly/release); needs orlyi, orlyc, orly_dm
#   ORLYI_PREFIX   command to run orlyi and orlyc under ("setarch x86_64 -R" for a TSan build)
#   EXTRA_ARGS     more orlyi flags (one named here replaces the default of the same name)
#
# Needs root for losetup and the /proc/partitions device scan, like restart_test.sh: it re-runs
# itself under sudo. Each run uses its own disk instance and detaches its loop device at exit.
# Ports 19940-19943; pass --port=<base> with non-overlapping ranges for parallel runs.
# Prints one summary line starting "GRACEFUL STOP:" and
# exits nonzero on any violation.
set -e
cd "$(dirname "$0")/.."
ORLY_OUT="$(cd "${ORLY_OUT:-$PWD/../out_orly/release}" && pwd)"
for bin in orly/server/orlyi orly/orlyc orly/indy/disk/util/orly_dm; do
  if [ ! -x "$ORLY_OUT/$bin" ]; then
    echo "missing: $ORLY_OUT/$bin (build it first, e.g. make release)"
    exit 1
  fi
done
if ! python3 -c 'import websocket' 2>/dev/null; then
  echo "graceful stop test: python3 can't import websocket; install websocket-client (pip3 install websocket-client)"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  # root's python3 doesn't see a user-level pip install, so hand it the directory this user's
  # python3 imports websocket-client from.
  PY_PATH="$(python3 -c 'import os, websocket; print(os.path.dirname(os.path.dirname(websocket.__file__)))')"
  exec sudo --preserve-env=STOPS,DEADLINE,LONG_STOPS,LONG_MAX_RUN,SEED,ORLYI_PREFIX,EXTRA_ARGS,TSAN_OPTIONS ORLY_OUT="$ORLY_OUT" \
    PYTHONPATH="$PY_PATH" python3 tests/graceful_stop_test.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/graceful_stop_test.py "$@"
