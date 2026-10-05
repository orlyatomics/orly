#!/bin/bash
# Disk-full smoke (#590): K=8 writers overwrite keys on a mem-sim orlyi whose
# volumes are tiny (MEM_MB fast, SLOW_MB slow), so free space runs low within a
# minute or two. orlyi must then refuse writes with "insufficient_storage",
# keep serving reads (on old and new sessions), and never abort.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with tiny volumes.
#   3. Run disk_full.mjs: write until refused, idle, then read and write.
#   4. Fail unless disk_full.mjs passed, orlyi is still alive, and its log
#      shows no abort of any kind.
#
# ADMISSION=off starts orlyi with write admission turned off (both reserve
# flags 0). It is the smoke's negative control: CI runs it and requires it to
# FAIL, so a change that stops the smoke from noticing missing admission (the
# disk never filling, say, or a refusal check that always passes) breaks CI
# instead of passing quietly.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19732
REPORT_PORT=19733
MEM_MB="${MEM_MB:-16}"
SLOW_MB="${SLOW_MB:-4}"
ADMISSION="${ADMISSION:-on}"
case "$ADMISSION" in
  on)  ADMISSION_FLAGS=() ;;
  off) ADMISSION_FLAGS=(--disk_reserve_mb=0 --disk_reserve_pct=0) ;;
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

"$ORLYI" --mem_sim --mem_sim_mb="$MEM_MB" --mem_sim_slow_mb="$SLOW_MB" --create=true \
         --port_number=19730 --slave_port_number=19731 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_disk_full_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=100000 --update_entry_pool_size=200000 \
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

echo "[3/3] K=8 writers until writes are refused, idle, then read and write (admission $ADMISSION)"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node disk_full.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 2
# Check the log before the process: an abort that has been logged may still be
# unwinding (or dumping core) when we look.
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "DISK FULL FAIL: orlyi logged an abort:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || true
  echo "DISK FULL FAIL: orlyi died"
  status=1
fi
for line in "^READ: ok" "^NEW SESSION READ: ok" "^DISK FULL OK"; do
  if ! grep -q "$line" "$WORK/smoke.out"; then
    echo "DISK FULL FAIL: missing \"$line\""
    status=1
  fi
done
grep -E "write admission:" "$WORK/orlyi.log" | head -4 || true
grep -cE "out of disk space" "$WORK/orlyi.log" | sed 's/^/out-of-space retry lines (rate-limited): /'
if [ "$status" -ne 0 ]; then
  echo "orlyi log (disk / merge / abort lines):"
  grep -E "out of disk|admission|StepMerge|StepTail|aborting|bad_alloc" "$WORK/orlyi.log" | sed 's/\[[0-9]*\]//g' | sort | uniq -c | sort -rn | head -20 || true
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
