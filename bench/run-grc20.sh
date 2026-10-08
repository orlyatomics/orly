#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
WORK="$REPO_ROOT/bench/.grc20-work-$$"
ORLYI_PID=""
mkdir "$WORK"
trap '
  if [ -n "$ORLYI_PID" ]; then
    kill -9 "$ORLYI_PID" 2>/dev/null || true
    wait "$ORLYI_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
' EXIT

(cd clients/ts && npm install --silent && npx tsc)
(cd "$WORK" && "$ORLY_OUT/orly/orlyc" -o "$WORK" "$REPO_ROOT/examples/grc20-pov/grc20.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/grc20.1.so" "$WORK/packages/"

"$ORLY_OUT/orly/server/orlyi" \
  --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
  --port_number=19900 --slave_port_number=19901 \
  --ws_port_number=19902 --reporting_port_number=19903 \
  --connection_backlog=32 --instance_name="release_grc20_$$" \
  --starting_state=SOLO --package_dir="$WORK/packages" --memory_budget_mb=1024 \
  > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

status=0
ORLY_URL=ws://127.0.0.1:19902/ node bench/grc20.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then status=1; fi
if grep -Eq 'FATAL ERROR|TERMINATE|aborting' "$WORK/orlyi.log"; then status=1; fi
if [ "$status" -ne 0 ]; then tail -30 "$WORK/orlyi.log"; fi
exit "$status"
