#!/bin/bash
# WebSocket concurrency smoke (#761): reads/s and p50/p99 latency of point reads and small prefix
# reads at 1, 3, 16, 64 and 256 WebSocket sessions, one connection each, against a fresh mem-sim
# orlyi with a fixed 1 GiB memory budget (unbudgeted, it sizes its pools from free RAM, #669).
# Every read is checked. Prints METRIC lines for tools/maint/ab_bench.py.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/smoke/ws_scale.orly with orlyc, start a fresh mem-sim orlyi.
#   2. Run ws_scale.mjs.
#   3. Check orlyi is still alive; kill it on exit.
# Environment: LEVELS, KINDS, SECS, GROUPS, WORKERS and SCALE_CHECK go to ws_scale.mjs (see
# there); EXTRA_ARGS adds orlyi flags, e.g. --num_ws_threads=1.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=${WS_PORT:-19772}
REPORT_PORT=${REPORT_PORT:-19773}
PORT=${PORT:-19770}
SLAVE_PORT=${SLAVE_PORT:-19771}

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

echo "[0/3] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=""
trap '[ -n "$ORLYI_PID" ] && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile ws_scale.orly and start fresh orlyi ${EXTRA_ARGS:-} (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/ws_scale.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/ws_scale.1.so" "$WORK/packages/"

# shellcheck disable=SC2086
"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT --slave_port_number=$SLAVE_PORT \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_ws_scale_smoke_$WS_PORT \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --memory_budget_mb=1024 \
         ${EXTRA_ARGS:-} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[2/3] wait for WebSocket port $WS_PORT"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[3/3] load, then read at each session count"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node ws_scale.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  echo "WS SCALE FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
