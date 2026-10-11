#!/bin/bash
# Exactly-once writes across a failover (#862): write with an operation id on a disk-backed master,
# fail over to its slave, and send the write again there. The checks are listed at the top of
# tests/failover_retry_test.py.
#
#   NEGATIVE=noretry
#                negative control: the slave's exactly-once table is off, so the retry runs again.
#                The run must FAIL, on the retry check.
#   ORLY_OUT     build tree (default ../out_orly/release); needs orlyi, orlyc, orly_dm
#
# Needs root for losetup and the /proc/partitions device scan, like kill_campaign.sh: it re-runs
# itself under sudo. Ports 19950-19953 and 19960-19963; pass --port=<base> to move them. Prints one
# summary line starting "FAILOVER RETRY:" and exits nonzero on any failure.
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
  echo "failover retry test: python3 can't import websocket; install websocket-client (pip3 install websocket-client)"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  PY_PATH="$(python3 -c 'import os, websocket; print(os.path.dirname(os.path.dirname(websocket.__file__)))')"
  exec sudo --preserve-env=NEGATIVE ORLY_OUT="$ORLY_OUT" PYTHONPATH="$PY_PATH" python3 tests/failover_retry_test.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/failover_retry_test.py "$@"
