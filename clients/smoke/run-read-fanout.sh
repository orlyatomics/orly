#!/bin/bash
# Point reads over many disk layers with a small fiber frame pool (#762).
# A single-key read used to take one fiber frame per disk layer, and when the pool ran dry part
# way through, the half-started read unwound under its running prep fibers: "pure virtual method
# called", and orlyi died. Now a read takes at most 16 frames and builds the rest inline.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly, start a mem-sim orlyi with the disk merges off
#      (so disk layers pile up), $FRAMES frames and 64 WebSocket threads.
#   2. read_fanout.mjs: load keys until the global repo has $LAYERS disk layers, then $R readers
#      read them back for $SECS s. Every read must answer, and orlyi must stay up.
# EXTRA_ARGS adds orlyi flags. KEEP_LOG=<path> keeps orlyi's log.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
PORT=${PORT:-19790}
SLAVE_PORT=${SLAVE_PORT:-19791}
WS_PORT=${WS_PORT:-19792}
REPORT_PORT=${REPORT_PORT:-19793}
FRAMES=${FRAMES:-200}
export LAYERS=${LAYERS:-40} R=${R:-128} SECS=${SECS:-15}

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

if [ -z "$SKIP_TS_BUILD" ]; then
  echo "[0/2] build clients/ts"
  (cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
fi

WORK="$(mktemp -d)"
ORLYI_PID=
cleanup() {
  if [ -n "$ORLYI_PID" ]; then kill -9 "$ORLYI_PID" 2>/dev/null || true; fi
  if [ -n "$KEEP_LOG" ]; then cp "$WORK/orlyi.log" "$KEEP_LOG" || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "[1/2] compile sample.orly and start orlyi with $FRAMES frames, disk merges off"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=${MEM_SIM_MB:-512} --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT --slave_port_number=$SLAVE_PORT \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=256 \
         --instance_name=orly_read_fanout \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --num_disk_merge_threads=0 \
         --num_ws_threads=64 \
         --max_parallel_frames=$FRAMES \
         ${EXTRA_ARGS} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[2/2] load to $LAYERS disk layers, then $R readers for $SECS s"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=$REPORT_PORT node read_fanout.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  echo "READ FANOUT FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  grep -v '^$' "$WORK/orlyi.log" | tail -25
fi
exit "$status"
