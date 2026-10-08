#!/bin/bash
# The POV review walkthrough (#746): compile grc20.orly, start a fresh orlyi, run review.py.
# Same shape as run.sh, on its own ports.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
PORT_BASE="${PORT_BASE:-19410}"
WS_PORT=$((PORT_BASE + 2))

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

echo "[1/5] compile grc20.orly (also runs inline tests)"
"$ORLYC" -o "$WORK" grc20.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/grc20.1.so" "$WORK/packages/"


echo "[3/5] start fresh orlyi (logs -> $WORK/orlyi.log)"
"$ORLYI" --mem_sim --create=true \
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$((PORT_BASE + 3)) \
         --connection_backlog=10 \
         --instance_name=grc20_pov_review \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         ${ORLYI_EXTRA_ARGS:-} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[4/5] wait for WebSocket port $WS_PORT"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
    break
  fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[5/5] run review.py"
ORLY_URL="ws://127.0.0.1:$WS_PORT/" PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 review.py
