#!/bin/bash
# Package-upgrade smoke (#797): once a package has stored and read records,
# installing another package that declares the same record type, or the next
# version of the same package, crashed orlyi on the next read of those
# records. package_upgrade.mjs installs xa.1, writes and reads records, then
# installs xb.1 (same source, another package), xa.2 (same source, next
# version) and xa.3 (the record type gained a field), reading through each.
# Every read must return exactly what was written, and orlyi must stay up.
#   0. Build the orly TS client (clients/ts).
#   1. Compile xa.1, xa.2, xa.3 and xb.1 with orlyc.
#   2. Start a fresh mem-sim orlyi.
#   3. Run package_upgrade.mjs.
#   4. Fail unless it passed and orlyi is still alive.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19880}"
WS_PORT=$((PORT_BASE + 2))
REPORT_PORT=$((PORT_BASE + 3))

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
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/3] compile xa.1, xa.2, xa.3, xb.1 and start fresh orlyi (logs -> $WORK/orlyi.log)"
SRC="$PWD/package_upgrade"
mkdir "$WORK/packages" "$WORK/v1" "$WORK/v2" "$WORK/v3" "$WORK/xb"
touch "$WORK/packages/__orly__"
cp "$SRC/xa.orly" "$WORK/v1/xa.orly"
sed 's/^package #1;/package #2;/' "$SRC/xa.orly" > "$WORK/v2/xa.orly"
cp "$SRC/xa_changed.orly" "$WORK/v3/xa.orly"
cp "$SRC/xb.orly" "$WORK/xb/xb.orly"
for dir in v1 v2 v3 xb; do
  (cd "$WORK/$dir" && "$ORLYC" --skip-tests -o "$WORK/$dir" "$WORK/$dir/"*.orly)
  cp "$WORK/$dir/"*.so "$WORK/packages/"
done
ls "$WORK/packages"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_package_upgrade_smoke \
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

echo "[3/3] install, write, read; install a second declarer, the next version, a changed type"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node package_upgrade.mjs > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
sleep 1
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  rc=0
  wait "$ORLYI_PID" || rc=$?
  echo "PACKAGE UPGRADE FAIL: orlyi died (exit $rc)"
  status=1
fi
if ! grep -q "^PACKAGE UPGRADE OK" "$WORK/smoke.out"; then
  echo "PACKAGE UPGRADE FAIL: missing \"PACKAGE UPGRADE OK\""
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
