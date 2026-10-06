#!/bin/bash
# Read-budget smoke (#694). A read method had no bound on the rows it walks or the memory it
# builds, so one read over a big range could take unbounded work or run orlyi out of memory.
# Now a call over the per-read budget is refused with "status": "read_too_large".
#   0. Build the orly TS client (clients/ts).
#   1. Compile read_budget.orly with orlyc.
#   2. Start orlyi with --read_budget_rows=$ROW_LIMIT --read_budget_mb=1, write $ROWS rows,
#      and require a range read over them, a loop of $ROWS point reads and a result bigger
#      than 1 MiB each to be refused as read_too_large (ReadTooLargeError in TS,
#      orly.ReadTooLarge in Python), reads under the budget to answer, and orlyi to stay up.
#   3. Control: the same reads against an orlyi with the budget off (--read_budget_mb=0)
#      succeed, so the refusals in step 2 are the budget's.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19782
export ROWS=30000 ROW_LIMIT=20000 BIG=400000

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

echo "[1/3] compile read_budget.orly"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/read_budget.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/read_budget.1.so" "$WORK/packages/"

start_orlyi() {
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=19780 --slave_port_number=19781 \
           --ws_port_number=$WS_PORT --reporting_port_number=19783 \
           --connection_backlog=10 \
           --instance_name=orly_read_budget_smoke \
           --starting_state=SOLO \
           --package_dir="$WORK/packages" \
           "$@" > "$WORK/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 60); do
    if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then return 0; fi
    sleep 1
  done
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
}

stop_orlyi() {
  kill -9 "$ORLYI_PID" 2>/dev/null || true
  wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
  for _ in $(seq 1 30); do
    if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then return 0; fi
    sleep 1
  done
}

export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
run_drivers() {
  PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 read_budget.py || status=$?
  node read_budget.mjs || status=$?
  if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
    echo "READ BUDGET FAIL: orlyi died"
    status=1
  fi
  if [ "$status" -ne 0 ]; then
    echo "orlyi log tail:"
    tail -20 "$WORK/orlyi.log"
    exit "$status"
  fi
}

echo "[2/3] reads over $ROW_LIMIT rows or 1 MiB are refused as read_too_large"
start_orlyi --read_budget_rows=$ROW_LIMIT --read_budget_mb=1
BUDGETED=1 run_drivers
stop_orlyi

echo "[3/3] control: with the budget off the same reads succeed"
start_orlyi --read_budget_mb=0
BUDGETED=0 run_drivers
exit "$status"
