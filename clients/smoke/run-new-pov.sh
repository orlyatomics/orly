#!/bin/bash
# Client new-POV smoke (#580): both drivers create every POV flavour
# (safe/fast x shared/private) and a child POV `from` a parent, against a
# live mem-sim orlyi, and write + read through each one. The other smokes
# only ever create the default `new safe shared pov;`, which is how the
# drivers shipped `new shared pov;` (no guarantee) and `parent "<id>"`
# (wrong keyword, quoted id), both syntax errors on the server.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with orlyc.
#   2. Start a fresh mem-sim orlyi on smoke-specific ports.
#   3. Run new_pov.py (Python driver) and new_pov.mjs (TS driver).
#   4. Kill orlyi on exit.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19712

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
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/3] compile sample.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"

pkill -9 -f 'instance_name=orly_new_pov_smoke' 2>/dev/null || true
sleep 1

"$ORLYI" --mem_sim --create=true \
         --port_number=19710 --slave_port_number=19711 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=orly_new_pov_smoke \
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

echo "[3/3] run both drivers"
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 new_pov.py
node new_pov.mjs
