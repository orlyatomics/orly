#!/bin/bash
# Write amplification and merge amplification smoke benchmark (#857).
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/smoke/write_amp.orly with orlyc, start a fresh mem-sim orlyi.
#   2. Run write_amp.mjs:
#      - Load 1: inserts of new keys (measures write & merge amp).
#      - Load 2: overwrites of a fixed key set across 3 passes.
#      - Checks overwrite write amplification limit and 3-pass flatness.
#   3. Check orlyi is still alive; kill it on exit.
# KEEP_LOG=<path> keeps orlyi's log. Prints METRIC lines for tools/maint/ab_bench.py.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
PORT_BASE="${PORT_BASE:-19740}"
PORT=$PORT_BASE
SLAVE_PORT=$((PORT_BASE + 1))
WS_PORT=$((PORT_BASE + 2))
REPORT_PORT=$((PORT_BASE + 3))

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

if [ -z "$SKIP_TS_BUILD" ]; then
  if command -v npm >/dev/null 2>&1; then
    echo "[0/3] build clients/ts"
    (cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
  fi
fi

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; [ -z "$KEEP_LOG" ] || cp "$WORK/orlyi.log" "$KEEP_LOG"; rm -rf "$WORK"' EXIT

echo "[1/3] compile write_amp.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/write_amp.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/write_amp.1.so" "$WORK/packages/"

echo "orlyi flags: ${ORLYI_FLAGS:-(none)}"

"$ORLYI" --mem_sim --mem_sim_mb=${MEM_SIM_MB:-256} --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT --replica_port_number=$SLAVE_PORT \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_write_amp_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         ${ORLYI_FLAGS} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[2/3] wait for WebSocket port $WS_PORT"
for _ in $(seq 1 60); do
  if (echo > /dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null || ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! (echo > /dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null && ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi


echo "[3/3] run write amplification and 3-pass flatness benchmark"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=$REPORT_PORT node write_amp.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -30 "$WORK/orlyi.log"
fi
exit "$status"
