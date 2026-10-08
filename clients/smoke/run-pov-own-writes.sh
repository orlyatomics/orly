#!/bin/bash
# POV read-your-own-writes smoke (#791): a POV reads its own deletes, overwrites and `+=` of keys
# an ancestor holds before they are promoted, for safe and fast, shared and private POVs, under a
# paused shared POV and under the global POV.
#   0. Build the orly TS client (clients/ts).
#   1. Compile pov_own_writes.orly with orlyc.
#   2. Start a fresh mem-sim orlyi.
#   3. Run pov_own_writes.mjs.
#   4. Fail unless it passed and orlyi is still alive.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19890}"
WS_PORT=$((PORT_BASE + 2))

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
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile pov_own_writes.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/pov_own_writes.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/pov_own_writes.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$((PORT_BASE + 3)) \
         --connection_backlog=10 \
         --instance_name=orly_pov_own_writes_smoke \
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

echo "[3/3] pov_own_writes.mjs"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node pov_own_writes.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  echo "POV OWN WRITES FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
