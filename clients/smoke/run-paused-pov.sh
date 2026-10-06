#!/bin/bash
# Paused-POV smoke (#626): K=8 writers write to one paused POV of a mem-sim
# orlyi with a small Update pool, so its writer backlog cap is 156 updates.
# A paused POV's backlog never drains until it is unpaused, and before the fix
# every write past that cap waited for it forever. Writes past the cap must be
# refused with insufficient_memory instead, reads must keep working, writes to
# other POVs must still be accepted, and after the unpause the POV's writes must
# be promoted and new ones accepted. orlyi must not abort.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with a 5000-update pool.
#   3. Run paused_pov.mjs.
#   4. Fail unless it passed, orlyi is still alive, and its log shows no abort.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19760}"
WS_PORT=$((PORT_BASE + 2))
REPORT_PORT=$((PORT_BASE + 3))

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
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_paused_pov_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=5000 --update_entry_pool_size=10000 \
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

echo "[3/3] K=8 writers on a paused POV, with a reader; a write to another POV; then unpause"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node paused_pov.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 2
# Check the log before the process: an abort that has been logged may still be
# unwinding when we look.
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "PAUSED POV FAIL: orlyi logged an abort:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || true
  echo "PAUSED POV FAIL: orlyi died"
  status=1
fi
for line in "^PAUSED POV CAPPED" "^OTHER POV WRITE: accepted" "^AFTER UNPAUSE: promoted, write accepted" "^PAUSED POV OK"; do
  if ! grep -q "$line" "$WORK/smoke.out"; then
    echo "PAUSED POV FAIL: missing \"$line\""
    status=1
  fi
done
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
