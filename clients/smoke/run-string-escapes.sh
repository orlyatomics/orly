#!/bin/bash
# String-escape smoke (#756): the Python, TS and Go clients write strings holding newlines,
# tabs, carriage returns, form feeds and other control characters through `call`, and read
# them back equal. The clients write those as \n, \r, \t and \xNN, which the lexer must accept.
#   0. Build the orly TS client (clients/ts).
#   1. Compile string_escapes.orly with orlyc.
#   2. Start a fresh mem-sim orlyi on smoke-specific ports.
#   3. Run string_escapes.py, string_escapes.mjs and string_escapes_go.
#   4. Kill orlyi on exit.
# The Go driver is skipped when `go` isn't installed, unless REQUIRE_GO=1.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19802

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
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile string_escapes.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/string_escapes.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/string_escapes.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19800 --slave_port_number=19801 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=orly_string_escapes_smoke \
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

echo "[3/3] run the drivers"
export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 string_escapes.py || status=$?
node string_escapes.mjs || status=$?
if command -v go >/dev/null 2>&1; then
  (cd string_escapes_go && go run .) || status=$?
elif [ "${REQUIRE_GO:-0}" = 1 ]; then
  echo "STRING ESCAPES FAIL: go is not installed and REQUIRE_GO=1"
  status=1
else
  echo "go: not installed, Go driver skipped"
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
