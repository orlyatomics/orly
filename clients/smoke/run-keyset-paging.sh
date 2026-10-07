#!/bin/bash
# Keyset-paging smoke (#735). `keys (T) @ <[...]> after <[last]> take n` seeks to the previous
# page's last key, so a page costs the rows it returns wherever it starts; `skip s take n` walks
# and throws away the s rows before the page, so page k costs k pages.
#   0. Build the orly TS client (clients/ts).
#   1. Compile keyset_paging.orly with orlyc.
#   2. Start orlyi with --read_budget_rows=$ROW_LIMIT and run keyset_paging.mjs, which writes
#      $ROWS edges and then
#        - times page 1 and a page $DEPTH rows in, both ways, and prints METRIC lines for
#          tools/maint/ab_bench.py: a keyset page that deep must cost about what page 1 does,
#          while the skip page grows with its depth;
#        - requires a skip page past $ROW_LIMIT rows to be refused as read_too_large while the
#          keyset page at the same place answers;
#        - pages through every edge with Client.pages and checks each comes back once, in order;
#        - writes and deletes rows between pages, before and after the boundary: rows after it
#          show up on later pages, rows before it don't;
#        - checks what a private POV with no writes of its own sees of its parent between pages.
#   3. Run keyset_paging.py and keyset_paging_go: the Python and Go page helpers walk the same
#      edges. The Go driver is skipped when `go` isn't installed, unless REQUIRE_GO=1.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19882
export ROWS="${ROWS:-30000}" ROW_LIMIT="${ROW_LIMIT:-20000}" PAGE="${PAGE:-100}"
export DEPTH="${DEPTH:-18000}"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

echo "[0/3] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile keyset_paging.orly"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/keyset_paging.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/keyset_paging.1.so" "$WORK/packages/"

echo "[2/3] start orlyi (read budget $ROW_LIMIT rows; logs -> $WORK/orlyi.log), run keyset_paging.mjs"
"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19880 --slave_port_number=19881 \
         --ws_port_number=$WS_PORT --reporting_port_number=19883 \
         --connection_backlog=10 \
         --instance_name=orly_keyset_paging_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --read_budget_rows=$ROW_LIMIT \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

export ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_REPORT_PORT=19883
status=0
node keyset_paging.mjs || status=$?

echo "[3/3] the Python and Go page helpers"
if [ "$status" -eq 0 ]; then
  PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 keyset_paging.py || status=$?
  if command -v go >/dev/null 2>&1; then
    (cd keyset_paging_go && go run .) || status=$?
  elif [ "${REQUIRE_GO:-0}" = 1 ]; then
    echo "KEYSET PAGING FAIL: go is not installed and REQUIRE_GO=1"
    status=1
  else
    echo "go: not installed, Go driver skipped"
  fi
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  echo "KEYSET PAGING FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
