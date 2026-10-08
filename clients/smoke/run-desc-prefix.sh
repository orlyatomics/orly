#!/bin/bash
# Desc-key prefix reads from disk files (#792): keys with a desc member (or a nested tuple
# holding one) before a free member, read back with a prefix pattern while in memory and again
# once the global repo is disk files only. Before the fix a release orlyi returned [] for every
# such read from disk, and a debug orlyi aborted on the disk walker's MatchPrefixState assert.
#   1. Compile clients/smoke/desc_prefix.orly with orlyc.
#   2. Start a fresh mem-sim orlyi.
#   3. Run desc_prefix.py.
#   4. Check orlyi is still alive; kill it on exit.
# ORLY_OUT picks the build (default debug, whose assert cross-checks every disk match).

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19792
REPORT_PORT=19793

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make debug"
    exit 1
  fi
done

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/3] compile desc_prefix.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/desc_prefix.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/desc_prefix.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19790 --slave_port_number=19791 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=10 \
         --instance_name=orly_desc_prefix_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[2/3] wait for WebSocket port $WS_PORT"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[3/3] write desc keys, read them from memory and from disk"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=$REPORT_PORT \
  PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 desc_prefix.py || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
