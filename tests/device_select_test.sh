#!/bin/bash
# Telling orlyi and orly_dm which devices to use (#970): two byte copies of one volume, on two
# loop devices, opened in parallel by two orlyi each given --device; with no --device, orlyi must
# refuse and name both. The steps are listed at the top of tests/device_select_test.py.
#
#   ORLY_OUT       build tree (default ../out_orly/debug); needs orlyi and orly_dm
#   ORLYI_PREFIX   command to run orlyi under ("setarch x86_64 -R" for a TSan build)
#   EXTRA_ARGS     more orlyi flags
#
# Needs root for losetup: it re-runs itself under sudo. Ports 19980-19995; pass --port=<base> for
# parallel runs. --ram=/dev/ramN also opens a volume made on that brd RAM disk (it is overwritten).
# Prints one summary line starting "DEVICE SELECT:" and exits nonzero on any violation.
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
  echo "device select test: python3 can't import websocket; install websocket-client (pip3 install websocket-client)"
  exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
  PY_PATH="$(python3 -c 'import os, websocket; print(os.path.dirname(os.path.dirname(websocket.__file__)))')"
  exec sudo --preserve-env=ORLYI_PREFIX,EXTRA_ARGS,TSAN_OPTIONS ORLY_OUT="$ORLY_OUT" \
    PYTHONPATH="$PY_PATH" python3 tests/device_select_test.py "$@"
fi
ORLY_OUT="$ORLY_OUT" exec python3 tests/device_select_test.py "$@"
