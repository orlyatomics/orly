#!/bin/bash
# End-to-end demo: a parimutuel prediction market on Orly, driven by the
# TypeScript driver on the shared `orly` client (clients/ts).
#   0. Build the orly TS client (clients/ts -> dist/).
#   1. Compile market.orly with orlyc.
#   2. Lay out a package directory orlyi can install from.
#   3. Start a fresh orlyi (killing any prior instance).
#   4. Build + run the driver (tsc demo.ts -> node demo.js): N traders bet
#      concurrently, prices fold from the trade log, time-travel replays the
#      price history, and the market resolves + pays out, with a self-check.
#   5. Kill orlyi on exit.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make debug"
    exit 1
  fi
done

if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
  echo "missing: node/npm (install Node.js 18+)"
  exit 1
fi

echo "[0/5] build the orly TS client (clients/ts)"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/5] compile market.orly"
"$ORLYC" -o "$WORK" market.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/market.0.so" "$WORK/packages/"

pkill -9 -f 'instance_name=prediction_market_demo' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi (logs -> $WORK/orlyi.log)"
"$ORLYI" --mem_sim --create=true \
         --port_number=19600 --slave_port_number=19601 \
         --connection_backlog=10 \
         --instance_name=prediction_market_demo \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[4/5] wait for WebSocket port 8082"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ':8082'; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ':8082'; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[5/5] build + run demo.ts"
npm install --silent
npx tsc
node demo.js
