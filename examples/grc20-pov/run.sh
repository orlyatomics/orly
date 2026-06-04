#!/bin/bash
# End-to-end driver: compile grc20.orly, start a fresh orlyi, run demo.py.
# Mirrors examples/wikipedia-categories/run.sh's shape.

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

echo "[1/5] compile grc20.orly (also runs inline tests)"
"$ORLYC" -o "$WORK" grc20.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/grc20.1.so" "$WORK/packages/"

pkill -9 -f 'instance_name=grc20_pov_demo' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi (logs -> $WORK/orlyi.log)"
"$ORLYI" --mem_sim --create=true \
         --port_number=19400 --slave_port_number=19401 \
         --connection_backlog=10 \
         --instance_name=grc20_pov_demo \
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
python3 demo.py
