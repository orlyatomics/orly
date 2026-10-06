#!/bin/bash
# Read-heavy smoke (#674): `+=` counters written in rounds, so their history
# spreads over memory layers and disk files, then several readers fold them
# back with point and range reads for a fixed time. Every read is checked.
# Prints reads per second as METRIC lines for tools/maint/ab_bench.py, which
# is what it is for: measuring a change to the read path.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/smoke/read_fold.orly with orlyc.
#   2. Start a fresh mem-sim orlyi.
#   3. Run read_fold.mjs.
#   4. Check orlyi is still alive; kill it on exit.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19752
REPORT_PORT=19753

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

echo "[1/3] compile read_fold.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/read_fold.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/read_fold.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19750 --slave_port_number=19751 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_read_fold_smoke \
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

echo "[3/3] write += counters, then read them for a while"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node read_fold.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
