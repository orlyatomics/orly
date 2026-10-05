#!/bin/bash
# Memory-full smoke (#607): K=8 writers send 200-write batches to a mem-sim
# orlyi whose update pools are forced small, so the Update Entry pool fills
# within seconds. orlyi must then refuse writes with "insufficient_memory",
# keep serving reads, accept writes again once its merges free the pools, and
# never abort.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with small update pools.
#   3. Run memory_full.mjs: write through refusals while reading, idle, then
#      write and read.
#   4. Fail unless memory_full.mjs passed, orlyi is still alive, and its log
#      shows no abort of any kind.
#
# ADMISSION=off starts orlyi with memory admission turned off
# (--memory_reserve_pct=0). It is the smoke's negative control: CI runs it and
# requires it to FAIL, so a change that stops the smoke from noticing missing
# admission (pools that never fill, say, or a refusal check that always
# passes) breaks CI instead of passing quietly.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19752
REPORT_PORT=19753
UPDATE_POOL="${UPDATE_POOL:-5000}"
ENTRY_POOL="${ENTRY_POOL:-10000}"
ADMISSION="${ADMISSION:-on}"
case "$ADMISSION" in
  on)  ADMISSION_FLAGS=() ;;
  off) ADMISSION_FLAGS=(--memory_reserve_pct=0) ;;
  *)   echo "ADMISSION must be on or off, not \"$ADMISSION\""; exit 1 ;;
esac

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
         --port_number=19750 --slave_port_number=19751 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_memory_full_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size="$UPDATE_POOL" --update_entry_pool_size="$ENTRY_POOL" \
         "${ADMISSION_FLAGS[@]}" \
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

echo "[3/3] K=8 writers of 200-write batches through refusals, with a reader; idle; then write and read (admission $ADMISSION)"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node memory_full.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 2
# Check the log before the process: an abort that has been logged may still be
# unwinding (or dumping core) when we look.
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "MEMORY FULL FAIL: orlyi logged an abort:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || true
  echo "MEMORY FULL FAIL: orlyi died"
  status=1
fi
for line in "^WRITE AFTER IDLE: accepted" "^NEW SESSION READ: ok" "^MEMORY FULL OK"; do
  if ! grep -q "$line" "$WORK/smoke.out"; then
    echo "MEMORY FULL FAIL: missing \"$line\""
    status=1
  fi
done
grep -E "memory admission:" "$WORK/orlyi.log" | head -2 || true
if [ "$status" -ne 0 ]; then
  echo "orlyi log (pool / merge / abort lines):"
  grep -E "admission|out of pool|bad_alloc|StepMerge|aborting|Tetris" "$WORK/orlyi.log" | sed 's/\[[0-9]*\]//g' | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -20 || true
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
