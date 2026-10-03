#!/bin/bash
# Disk-full smoke (#590): K=8 writers overwrite keys on a mem-sim orlyi whose
# volumes are tiny (MEM_MB fast, SLOW_MB slow), so every volume fills within a
# minute or two. Before #590 a full disk aborted orlyi from inside a merge; now
# a merge that runs out of space hands its inputs back and retries.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with tiny volumes.
#   3. Run disk_full.mjs: write until the log shows a merge retrying for space,
#      a little longer, then read.
#   4. Fail on any merge abort. If orlyi is alive, the read must have worked.
#      If it died, it must have been one of the full-disk aborts still left
#      (durable writer/merger, file-service base image), which waiting for
#      space or refusing writes early (#590 step 4) has yet to remove.

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

echo "[3/3] K=8 writers until the disk is full, then a read"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLYI_LOG="$WORK/orlyi.log" node disk_full.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 2
MERGE_ABORT='StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception'
KNOWN_ABORT='TDurableManager::Run(Writer|Merger) .*aborting|TFileService base image .*aborting'
if grep -Eq "$MERGE_ABORT" "$WORK/orlyi.log"; then
  echo "DISK FULL FAIL: a merge aborted:"
  grep -E "$MERGE_ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if [ "$status" -eq 0 ]; then
  if kill -0 "$ORLYI_PID" 2>/dev/null; then
    if ! grep -q "^READ: ok" "$WORK/smoke.out"; then
      echo "DISK FULL FAIL: orlyi is up but the read failed"
      status=1
    fi
  else
    wait "$ORLYI_PID" || true
    if grep -Eq "$KNOWN_ABORT" "$WORK/orlyi.log"; then
      echo "NOTE: orlyi died in a full-disk abort that is still expected (#590 step 4):"
      grep -E "$KNOWN_ABORT" "$WORK/orlyi.log" | head -1
    else
      echo "DISK FULL FAIL: orlyi died, and not in a known full-disk abort"
      status=1
    fi
  fi
fi
grep -cE "inputs handed back" "$WORK/orlyi.log" | sed 's/^/merge retry lines (rate-limited): /'
if [ "$status" -ne 0 ]; then
  echo "orlyi log (disk / merge / abort lines):"
  grep -E "out of disk|StepMerge|StepTail|aborting|bad_alloc" "$WORK/orlyi.log" | sed 's/\[[0-9]*\]//g' | sort | uniq -c | sort -rn | head -20 || true
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
