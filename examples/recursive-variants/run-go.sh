#!/bin/bash
# End-to-end WS smoke test for recursive sum-type storage + client
# marshaling (issue #115), driven by the go driver:
#   1. Compile recursive.orly with orlyc.
#   2. Lay out a package directory orlyi can install from.
#   3. Start a fresh orlyi (killing any prior instance).
#   4. Run the driver: store a recursive value of each shape over the
#      WebSocket protocol, read it back, and self-check the marshaled JSON.
#   5. Kill orlyi on exit.
#
# Run from any working directory; binaries are found relative to the repo,
# two levels up. Override with ORLY_OUT if your build went elsewhere.

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

if ! command -v go >/dev/null 2>&1; then
  echo "missing: go (install golang-go or download from go.dev)"
  exit 1
fi

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/5] compile recursive.orly"
"$ORLYC" -o "$WORK" recursive.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/recursive.0.so" "$WORK/packages/"

pkill -9 -f 'instance_name=recursive_variants_demo' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi (logs -> $WORK/orlyi.log)"
"$ORLYI" --mem_sim --create=true \
         --port_number=19600 --slave_port_number=19601 \
         --connection_backlog=10 \
         --instance_name=recursive_variants_demo \
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

echo "[5/5] go run demo.go"
go run demo.go
