#!/bin/bash
# Batch-promotion smoke (#751): batched calls (callBatch/callMany, sizes 1, 2
# and 8) to methods with conditional effects, conditional values, assert-only
# reads and plain writes, on safe/fast x shared/private POVs. Every write is
# checked after Tetris has promoted it to the global POV, and each POV must
# still take writes afterwards. Before #751, any batched call with an `if`
# replied ok and then failed promotion: its writes were lost and its POV failed.
#   0. Build the orly TS client (clients/ts).
#   1. Compile batch_promotion.orly with orlyc.
#   2. Start a fresh mem-sim orlyi on smoke-specific ports.
#   3. Run the Python, TS and Go drivers, one after another (the Go driver is
#      skipped when `go` isn't installed, unless REQUIRE_GO=1).
#   4. Kill orlyi on exit.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19752

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

echo "[1/3] compile batch_promotion.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/batch_promotion.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/batch_promotion.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19750 --slave_port_number=19751 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=orly_batch_promotion_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" --le --log_info \
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

echo "[3/3] run the drivers"
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 batch_promotion.py || status=$?
node batch_promotion.mjs || status=$?
if command -v go >/dev/null 2>&1; then
  (cd batch_promotion_go && go run .) || status=$?
elif [ "${REQUIRE_GO:-0}" = 1 ]; then
  echo "BATCH PROMOTION FAIL: go is not installed and REQUIRE_GO=1"
  status=1
else
  echo "go: not installed, Go driver skipped"
fi
# The server logs a failed promotion replay; none may appear.
if grep -E "exception while testing assertions|Failing Repo" "$WORK/orlyi.log"; then
  echo "BATCH PROMOTION FAIL: orlyi logged a failed promotion (above)"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
