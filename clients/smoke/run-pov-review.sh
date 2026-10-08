#!/bin/bash
# POV review smoke (#746): diff a POV against its parent (also while the parent is written,
# restricted to a key range, and paged), report and refuse promotion conflicts, and discard a
# POV's changes, releasing their memory; for safe and fast, shared and private POVs, under the
# global POV and under a paused shared POV.
#   0. Build the orly TS client (clients/ts).
#   1. Compile pov_review.orly with orlyc.
#   2. Start a fresh mem-sim orlyi with update pools big enough for a 4,000-write paused POV.
#   3. Run pov_review.mjs, then pov_review.py and pov_review_go (the Python and Go clients).
#   4. Fail unless it passed, orlyi is still alive, and its log shows no abort.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19900}"
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
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile pov_review.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/pov_review.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/pov_review.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=10 \
         --instance_name=orly_pov_review_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=${UPDATE_POOL_SIZE:-200000} --update_entry_pool_size=${UPDATE_ENTRY_POOL_SIZE:-400000} \
         ${ORLYI_EXTRA_ARGS:-} \
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

echo "[3/3] pov_review.mjs, then the Python and Go drivers"
status=0
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
ORLY_REPORT_PORT=$REPORT_PORT node pov_review.mjs || status=$?
PYTHONPATH="$REPO_ROOT/clients/python:${PYTHONPATH:-}" python3 pov_review.py || status=$?
if command -v go >/dev/null 2>&1; then
  (cd pov_review_go && go run .) || status=$?
elif [ "${REQUIRE_GO:-0}" = 1 ]; then
  echo "POV REVIEW FAIL: go is not installed and REQUIRE_GO=1"
  status=1
else
  echo "go: not installed, Go driver skipped"
fi
sleep 1
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "POV REVIEW FAIL: orlyi logged an abort:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  echo "POV REVIEW FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
