#!/bin/bash
# End-to-end smoke for the claims model: compile claims.orly (which runs its
# inline tests), start a fresh orlyi, and run claims_demo.py, which checks
# every result and exits non-zero on any mismatch.
#
# Environment (all optional):
#   ORLY_OUT          debug build tree (default ../out_orly/debug next to the repo)
#   ORLY_WS_PORT      WebSocket port (default 8082); the other ports follow it
#   ORLYI_EXTRA_ARGS  extra orlyi flags, e.g. --memory_budget_mb=1024

set -e

cd "$(dirname "$0")"
HERE="$PWD"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT="${ORLY_WS_PORT:-8082}"

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
ORLYI_PID=
trap '[ -n "$ORLYI_PID" ] && kill -9 "$ORLYI_PID" 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/4] compile claims.orly (also runs its inline tests)"
# orlyc writes its intermediates to the current directory, so run it from $WORK.
(cd "$WORK" && "$ORLYC" -o "$WORK" "$HERE/claims.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/claims.1.so" "$WORK/packages/"

echo "[2/4] start fresh orlyi on ws port $WS_PORT (logs -> $WORK/orlyi.log)"
# shellcheck disable=SC2086
"$ORLYI" --mem_sim --create=true \
         --port_number=$((WS_PORT + 11300)) --slave_port_number=$((WS_PORT + 11301)) \
         --reporting_port_number=$((WS_PORT + 11302)) --ws_port_number="$WS_PORT" \
         --connection_backlog=10 \
         --instance_name=grc20_claims_demo \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         ${ORLYI_EXTRA_ARGS:-} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[3/4] wait for the WebSocket port"
for _ in $(seq 1 90); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then
    break
  fi
  if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
    break
  fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[4/4] run claims_demo.py"
ORLY_WS_URL="ws://127.0.0.1:$WS_PORT/" \
  PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 claims_demo.py
