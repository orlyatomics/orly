#!/bin/bash
# Write-too-large smoke (#687). A single write with more entries than half the
# Update Entry pool's merge reserve can never be promoted, so orlyi refuses it
# with "status": "write_too_large", which, unlike insufficient_memory, is not
# retryable. Against a mem-sim orlyi with a 10,000-entry pool and the default
# 25% reserve (2,500 entries, so a limit of 1,250), both drivers send a batch
# of 2 * LIMIT + 2 entries and require the typed error (WriteTooLargeError in
# TS, orly.WriteTooLarge in Python), then send the same rows in batches of
# LIMIT and require them all to land.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc and start orlyi.
#   2. Run write_too_large.py and write_too_large.mjs.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19772
ENTRY_POOL=10000
RESERVE_PCT=25
# CheckMemoryAdmission refuses entries * 2 > reserve.
export LIMIT=$((ENTRY_POOL * RESERVE_PCT / 100 / 2))

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

echo "[0/2] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/2] compile sample.orly and start orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"
"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19770 --slave_port_number=19771 \
         --ws_port_number=$WS_PORT --reporting_port_number=19773 \
         --connection_backlog=10 \
         --instance_name=orly_write_too_large_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_entry_pool_size=$ENTRY_POOL --memory_reserve_pct=$RESERVE_PCT \
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

echo "[2/2] oversized batch refused as write_too_large (limit $LIMIT entries); split batches accepted"
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 write_too_large.py || status=$?
node write_too_large.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  echo "WRITE TOO LARGE FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
