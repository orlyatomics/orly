#!/bin/bash
# Headless browser smoke test (Playwright): two tabs trade concurrently on one
# market and the price moves live. Starts orlyi + the static server, runs the
# Playwright spec, tears everything down.
#
# The Playwright browser must be installed first (CI does this):
#   npx playwright install --with-deps chromium
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"; ORLYC="$ORLY_OUT/orly/orlyc"
for bin in "$ORLYI" "$ORLYC"; do
  [ -x "$bin" ] || { echo "missing: $bin (run 'make debug')"; exit 1; }
done
command -v node >/dev/null && command -v npm >/dev/null || { echo "missing node/npm"; exit 1; }

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID $HTTP_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/4] build orly TS client + install web deps"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
npm install --silent

echo "[2/4] compile market.orly + start orlyi"
"$ORLYC" -o "$WORK" ../market.orly
mkdir "$WORK/packages"; touch "$WORK/packages/__orly__"; cp "$WORK/market.0.so" "$WORK/packages/"
pkill -9 -f 'instance_name=prediction_market_e2e' 2>/dev/null || true; sleep 1
"$ORLYI" --mem_sim --create=true --port_number=19600 --slave_port_number=19601 \
  --connection_backlog=10 --instance_name=prediction_market_e2e --starting_state=SOLO \
  --package_dir="$WORK/packages" > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!
for _ in $(seq 1 60); do ss -tln 2>/dev/null | grep -q ':8082' && break; sleep 1; done
ss -tln 2>/dev/null | grep -q ':8082' || { echo "orlyi failed:"; tail -20 "$WORK/orlyi.log"; exit 1; }

echo "[3/4] build bundle + serve on :8000"
./build.sh
python3 -m http.server 8000 >/dev/null 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 40); do ss -tln 2>/dev/null | grep -q ':8000' && break; sleep 0.5; done

echo "[4/4] run Playwright"
npx playwright test
