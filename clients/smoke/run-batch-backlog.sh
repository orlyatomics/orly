#!/bin/bash
# Batch-backlog smoke (#628): K=8 writers send 200-write batches to one POV of
# a mem-sim orlyi with a 20000-update / 40000-entry pool and a 25% memory
# reserve. With the writer backlog capped only in updates, the batches' entries
# filled the Entry pool: merges and Tetris missed, and writers were refused.
# With it capped in entries too, no batch may be refused, no pool may miss, and
# the Entry pool must stay under half full. orlyi must not abort.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with 20000/40000 pools, reserve RESERVE_PCT.
#   3. Run batch_backlog.mjs.
#   4. Fail unless it passed, orlyi is still alive, and its log shows no abort.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19780}"
RESERVE_PCT="${RESERVE_PCT:-25}"
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
         --instance_name=orly_batch_backlog_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=20000 --update_entry_pool_size=40000 \
         --memory_reserve_pct="$RESERVE_PCT" \
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

echo "[3/3] K=8 writers of 200-write batches on one POV, memory reserve $RESERVE_PCT%"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" REPORT_PORT=$REPORT_PORT node batch_backlog.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 2
# Check the log before the process: an abort that has been logged may still be
# unwinding when we look.
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "BATCH BACKLOG FAIL: orlyi logged an abort:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || true
  echo "BATCH BACKLOG FAIL: orlyi died"
  status=1
fi
for line in "^BATCH BACKLOG OK"; do
  if ! grep -q "$line" "$WORK/smoke.out"; then
    echo "BATCH BACKLOG FAIL: missing \"$line\""
    status=1
  fi
done
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
