#!/bin/bash
# Disk file count under sustained writes, and read latency against it (#701).
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc, start a fresh mem-sim orlyi.
#   2. Run layer_count.mjs: K writers add new keys for SECS seconds while one reader reads them.
#   3. Check orlyi is still alive; kill it on exit.
# ORLYI_FLAGS adds orlyi flags: --num_disk_merge_threads=1 makes the file count spike while one
# big merge holds the only merge thread, and =0 lets it grow without bound (reads at ~300 files).
# KEEP_LOG=<path> keeps orlyi's log. Prints METRIC lines for tools/maint/ab_bench.py.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=${WS_PORT:-19732}
REPORT_PORT=${REPORT_PORT:-19733}
PORT=${PORT:-19730}
SLAVE_PORT=${SLAVE_PORT:-19731}

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

if [ -z "$SKIP_TS_BUILD" ]; then
  echo "[0/3] build clients/ts"
  (cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
fi

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; [ -z "$KEEP_LOG" ] || cp "$WORK/orlyi.log" "$KEEP_LOG"; rm -rf "$WORK"' EXIT

echo "[1/3] compile sample.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"

echo "orlyi flags: ${ORLYI_FLAGS:-(none)}"

"$ORLYI" --mem_sim --mem_sim_mb=${MEM_SIM_MB:-512} --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT --slave_port_number=$SLAVE_PORT \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_layer_count \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         ${ORLYI_FLAGS} \
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

echo "[3/3] K=${K:-8} writers and one reader for ${SECS:-60} s"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=$REPORT_PORT node layer_count.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
