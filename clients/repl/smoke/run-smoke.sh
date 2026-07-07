#!/bin/bash
# Smoke test for orly-repl (#535), mirroring clients/mcp/smoke/run-smoke.sh:
#   0. Build the orly TS client (clients/ts) and this package (clients/repl).
#   1. Lay out an empty package directory (the REPL compiles its own package).
#   2. Start a fresh mem-sim orlyi on smoke-specific ports.
#   3. Run smoke.mjs: drive the REPL binary over stdin and assert on its
#      output — expression eval, definitions (incl. multi-line), cross-def
#      calls, storage write/read-back, compile-error recovery.
#   4. Kill orlyi on exit.

set -e

cd "$(dirname "$0")/.."
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

if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
  echo "missing: node/npm (install Node.js 18+)"
  exit 1
fi

echo "[0/3] build clients/ts and clients/repl"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
# npm copies file:../ts at install time and won't refresh an existing copy
# (same version number), so drop it to pick up the just-built driver.
rm -rf node_modules/orly
npm install --silent
npx tsc

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/3] lay out packages/ and start fresh orlyi (logs -> $WORK/orlyi.log)"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"

pkill -9 -f 'instance_name=orly_repl_smoke' 2>/dev/null || true
sleep 1

"$ORLYI" --mem_sim --create=true \
         --port_number=19750 --slave_port_number=19751 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=orly_repl_smoke \
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

echo "[3/3] drive the REPL"
ORLY_URL="ws://127.0.0.1:$WS_PORT/" \
ORLY_PACKAGE_DIR="$WORK/packages" \
ORLYC="$ORLYC" \
node smoke/smoke.mjs
