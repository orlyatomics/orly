#!/bin/bash
# Insert-order smoke (#754): batches of 4096 new keys through one POV, in key
# order and then at random, each mode against its own fresh mem-sim orlyi.
# A memory layer insert used to find its place by walking back from the
# layer's tail, so random keys (uuids, hashes) got slower the more the layer
# held; in-order keys stayed flat. Prints keys per second and late batch
# times per mode as METRIC lines for tools/maint/ab_bench.py; reads back a
# sample of the keys and fails on a wrong value.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/smoke/insert_order.orly with orlyc.
#   2. For MODES (default "ordered random"): start a fresh orlyi, run
#      insert_order.mjs, check orlyi is still alive, kill it.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19754
REPORT_PORT=19755
MODES="${MODES:-ordered random}"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

echo "[0/2] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'if [ -n "$ORLYI_PID" ]; then kill -9 $ORLYI_PID 2>/dev/null || true; fi; rm -rf "$WORK"' EXIT

echo "[1/2] compile insert_order.orly"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/insert_order.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/insert_order.1.so" "$WORK/packages/"

status=0
for mode in $MODES; do
  echo "[2/2] $mode keys against a fresh orlyi (logs -> $WORK/orlyi.$mode.log)"
  "$ORLYI" --mem_sim --mem_sim_mb=1024 --mem_sim_slow_mb=256 --create=true \
           --update_pool_size=1000000 --update_entry_pool_size=1000000 \
           --port_number=19756 --slave_port_number=19757 \
           --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
           --instance_name=orly_insert_order_smoke \
           --starting_state=SOLO \
           --package_dir="$WORK/packages" \
           > "$WORK/orlyi.$mode.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 60); do
    if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
    sleep 1
  done
  if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
    echo "orlyi failed to come up; last log lines:"
    tail -20 "$WORK/orlyi.$mode.log"
    exit 1
  fi
  mode_status=0
  MODE=$mode ORLY_URL="ws://127.0.0.1:$WS_PORT/" node insert_order.mjs || mode_status=$?
  if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
    wait "$ORLYI_PID" || echo "orlyi exited with status $?"
    mode_status=1
  fi
  if [ "$mode_status" -ne 0 ]; then
    tail -20 "$WORK/orlyi.$mode.log"
    status=$mode_status
  fi
  kill -9 "$ORLYI_PID" 2>/dev/null || true
  wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
  # Let the ports go before the next mode's orlyi binds them.
  for _ in $(seq 1 30); do
    if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
    sleep 1
  done
done
exit "$status"
