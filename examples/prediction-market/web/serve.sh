#!/bin/bash
# One command to try the prediction market in a browser:
#   - compiles market.orly, starts a fresh orlyi with it,
#   - builds the browser bundle,
#   - serves this directory; open the printed URL in two tabs.
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
for bin in "$ORLYI" "$ORLYC"; do
  [ -x "$bin" ] || { echo "missing: $bin (run 'make debug')"; exit 1; }
done

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID $HTTP_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/4] compile market.orly"
"$ORLYC" -o "$WORK" ../market.orly
mkdir "$WORK/packages"; touch "$WORK/packages/__orly__"; cp "$WORK/market.0.so" "$WORK/packages/"

echo "[2/4] start orlyi"
pkill -9 -f 'instance_name=prediction_market_web' 2>/dev/null || true; sleep 1
"$ORLYI" --mem_sim --create=true --port_number=19600 --slave_port_number=19601 \
  --connection_backlog=10 --instance_name=prediction_market_web --starting_state=SOLO \
  --package_dir="$WORK/packages" > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!
for _ in $(seq 1 60); do ss -tln 2>/dev/null | grep -q ':8082' && break; sleep 1; done
ss -tln 2>/dev/null | grep -q ':8082' || { echo "orlyi failed:"; tail -20 "$WORK/orlyi.log"; exit 1; }

echo "[3/4] build the browser bundle"
./build.sh

echo "[4/4] serve on http://localhost:8000  (Ctrl-C to stop)"
echo "  -> open http://localhost:8000 in TWO tabs to trade against yourself"
python3 -m http.server 8000 >/dev/null 2>&1 &
HTTP_PID=$!
wait $HTTP_PID
