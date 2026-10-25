#!/bin/bash
# The replication flags renamed in #1034 keep working under their old names.
#   1. orlyi given --slave_port_number and --slave_bind_address: the
#      replication listener is bound where the old flags put it, and the log
#      names each deprecated flag and its new spelling.
#   2. orlyi given --replica_port_number and --replica_bind_address: the same
#      listener, and no deprecation warning.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
PORT=19800
REPLICA_PORT=19801
WS_PORT=19802
REPORTING_PORT=19803

if [ ! -x "$ORLYI" ]; then
  echo "missing: $ORLYI"
  echo "Build the project first (from $REPO_ROOT):  make release"
  exit 1
fi

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"

fail() {
  echo "DEPRECATED FLAGS FAIL: $*"
  tail -20 "$WORK/orlyi.log"
  exit 1
}

bound() {
  ss -Htln "sport = :$1" 2>/dev/null | awk '{print $4}' | head -1
}

wait_ports_free() {
  local deadline=$((SECONDS + 30))
  while [ $SECONDS -lt $deadline ]; do
    if [ -z "$(bound $PORT)" ] && [ -z "$(bound $WS_PORT)" ] && [ -z "$(bound $REPORTING_PORT)" ] &&
       [ -z "$(bound $REPLICA_PORT)" ]; then return; fi
    sleep 0.1
  done
  echo "DEPRECATED FLAGS FAIL: ports still in use after 30 seconds"
  exit 1
}

start_orlyi() {
  wait_ports_free
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=$PORT --ws_port_number=$WS_PORT --reporting_port_number=$REPORTING_PORT \
           --connection_backlog=10 --instance_name=orly_deprecated_flags_smoke \
           --starting_state=SOLO --package_dir="$WORK/packages" --le "$@" \
           > "$WORK/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 60); do
    if [ -n "$(bound $PORT)" ] && [ -n "$(bound $REPLICA_PORT)" ]; then return; fi
    sleep 1
  done
  fail "orlyi failed to open its listeners"
}

stop_orlyi() {
  kill -9 "$ORLYI_PID" 2>/dev/null || true
  wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
}

echo "[1/2] old names: --slave_port_number, --slave_bind_address"
start_orlyi --slave_port_number=$REPLICA_PORT --slave_bind_address=127.0.0.1
got="$(bound $REPLICA_PORT)"
echo "  replication listener: $got"
[ "$got" = "127.0.0.1:$REPLICA_PORT" ] || fail "replication listener bound to '$got'"
grep -q -- "--slave_port_number is deprecated; use --replica_port_number" "$WORK/orlyi.log" ||
  fail "no deprecation warning for --slave_port_number"
grep -q -- "--slave_bind_address is deprecated; use --replica_bind_address" "$WORK/orlyi.log" ||
  fail "no deprecation warning for --slave_bind_address"
stop_orlyi

echo "[2/2] new names: --replica_port_number, --replica_bind_address"
start_orlyi --replica_port_number=$REPLICA_PORT --replica_bind_address=127.0.0.1
got="$(bound $REPLICA_PORT)"
echo "  replication listener: $got"
[ "$got" = "127.0.0.1:$REPLICA_PORT" ] || fail "replication listener bound to '$got'"
if grep -q "deprecated" "$WORK/orlyi.log"; then fail "a deprecation warning for the new names"; fi
stop_orlyi

echo "DEPRECATED FLAGS OK"
