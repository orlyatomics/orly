#!/bin/bash
# A restart keeps the saved-repo entries of the safe POVs that WAL recovery restores (#966): make
# safe POVs and a fast one on a disk-backed orlyi on a loopback volume, stop and open the store
# twice, and require no restored POV's entry to be removed, the fast POV's to be, and at most one
# entry of a gone repo to come back on the second open. Details at the top of
# tests/saved_repo_restart_test.py.
#
#   ORLY_OUT       build tree (default ../out_orly/debug); needs orlyi and orly_dm
#   ORLYI_PREFIX   command to run orlyi under ("setarch x86_64 -R" for a TSan build)
#   EXTRA_ARGS     more orlyi flags (one named here replaces the default of the same name)
#
# Needs root for losetup, like restart_test.sh: it re-runs itself under sudo. Ports 19980-19983;
# pass --port=<base> for parallel runs. Prints one summary line starting "SAVED REPO RESTART:" and
# exits nonzero on any violation.
set -e
cd "$(dirname "$0")/.."
ORLY_OUT="$(cd "${ORLY_OUT:-$PWD/../out_orly/debug}" && pwd)"
for bin in orly/server/orlyi orly/indy/disk/util/orly_dm; do
  if [ ! -x "$ORLY_OUT/$bin" ]; then
    echo "missing: $ORLY_OUT/$bin (build it first, e.g. make debug)"
    exit 1
  fi
done
if ! python3 -c 'import websocket' 2>/dev/null; then
  echo "saved repo restart test: python3 can't import websocket; install websocket-client (pip3 install websocket-client)"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  PY_PATH="$(python3 -c 'import os, websocket; print(os.path.dirname(os.path.dirname(websocket.__file__)))')"
  exec sudo --preserve-env=ORLYI_PREFIX,EXTRA_ARGS,TSAN_OPTIONS ORLY_OUT="$ORLY_OUT" \
    PYTHONPATH="$PY_PATH" python3 tests/saved_repo_restart_test.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/saved_repo_restart_test.py "$@"
