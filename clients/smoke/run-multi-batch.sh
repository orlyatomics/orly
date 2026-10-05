#!/bin/bash
# Mixed-batch smoke (#255): both drivers run several different methods of a
# package in one transaction (`try {pov} [pkg m1 <{...}>, pkg m2 <{...}>];`)
# against a live mem-sim orlyi, and check that every write lands, that the
# per-call results keep their own types, that a batch with a bad call leaves
# nothing behind, and that calls read the pre-batch snapshot.
#   0. Build the orly TS client (clients/ts).
#   1. Compile multi.orly with orlyc.
#   2. Start a fresh mem-sim orlyi on smoke-specific ports.
#   3. Run multi_batch.py (Python driver) and multi_batch.mjs (TS driver).
#   4. Kill orlyi on exit.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19742

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make debug"
    exit 1
  fi
done

echo "[0/3] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile multi.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/multi.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/multi.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19740 --slave_port_number=19741 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=orly_multi_batch_smoke \
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

echo "[3/3] run both drivers"
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 multi_batch.py || status=$?
node multi_batch.mjs || status=$?
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
