#!/bin/bash
# Concurrent-writer pool-pressure smoke (#584): K=8 writers against a mem-sim
# orlyi whose Update pool is forced small, so the run doesn't depend on how
# much RAM the host has free (the pool is otherwise sized from it). Before the
# fix this aborted orlyi within seconds: the repo layer cleaner wedged on the
# Tetris runner, dead layers and their update copies piled up, and a merge
# step's bad_alloc called abort().
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with a 20000-update pool.
#   3. Run pool_pressure.mjs (shared POV, then a POV per writer).
#   4. Check orlyi is still alive; kill it on exit.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19722
REPORT_PORT=19723

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
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/3] compile sample.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19720 --slave_port_number=19721 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_pool_pressure_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=20000 --update_entry_pool_size=40000 \
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

echo "[3/3] K=8 writers, shared POV then a POV per writer"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=$REPORT_PORT node pool_pressure.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log (pool / merge / Tetris lines):"
  grep -E "bad_alloc|StepMergeMem|Tetris|out of disk|out of pool" "$WORK/orlyi.log" | sort | uniq -c | head -20 || true
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
