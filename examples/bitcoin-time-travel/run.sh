#!/bin/bash
# Drives the end-to-end demo:
#   1. Compile bitcoin.orly with orlyc (also runs its inline tests).
#   2. Lay out a package directory orlyi can install from.
#   3. Start a fresh orlyi (killing any prior `bitcoin_demo` instance).
#   4. Run demo.py, which connects via WebSocket, applies the chain,
#      queries balances, and self-checks against expected values.
#   5. Kill orlyi on exit.
#
# Run from any working directory; binaries are found relative to the
# repo, which is two levels up from this script. Override with the
# ORLY_OUT environment variable if your build went elsewhere.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo ""
    echo "Build the project first (from $REPO_ROOT):"
    echo "  make debug"
    echo ""
    echo "Or set ORLY_OUT to point at your debug-build output tree."
    exit 1
  fi
done

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/5] compile bitcoin.orly (also runs inline tests)"
"$ORLYC" -o "$WORK" bitcoin.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/bitcoin.1.so" "$WORK/packages/"

# Stop anything that might already be using port 8082 or the instance name.
pkill -9 -f 'instance_name=bitcoin_time_travel_demo' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi (logs -> $WORK/orlyi.log)"
"$ORLYI" --mem_sim --create=true \
         --port_number=19400 --slave_port_number=19401 \
         --connection_backlog=10 \
         --instance_name=bitcoin_time_travel_demo \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[4/5] wait for WebSocket port 8082"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ':8082'; then
    break
  fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ':8082'; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[5/5] run demo.py"
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 demo.py
