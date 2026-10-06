#!/bin/bash
# Listener and remote-compile defaults (#705). Orly has no authentication, so
# on a bare host orlyi binds its client, WebSocket and reporting listeners to
# 127.0.0.1, keeps the replication listener on every interface (a slave on
# another host must reach it), and refuses the compile statement over
# WebSocket with "status": "remote_compile_disabled".
#   1. orlyi with no bind flags: each listener is bound where its default
#      says, the startup log names each bound address, a connection to this
#      host's non-loopback address is refused, and compile is refused with the
#      typed status (orly.RemoteCompileDisabled).
#   2. orlyi with --bind_address=0.0.0.0 --allow_remote_compile: the same
#      listeners on every interface, reachable on the non-loopback address,
#      and compile is no longer refused as disabled.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
PORT=19790
SLAVE_PORT=19791
WS_PORT=19792
REPORTING_PORT=19793

if [ ! -x "$ORLYI" ]; then
  echo "missing: $ORLYI"
  echo "Build the project first (from $REPO_ROOT):  make release"
  exit 1
fi

# This host's first non-loopback IPv4 address, if it has one.
OTHER_IP="$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' | grep -v '^127\.' | head -1 || true)"

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"

fail() {
  echo "BIND DEFAULTS FAIL: $*"
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
  exit 1
}

# The local address the listener on port $1 is bound to, from ss.
bound() {
  ss -Htln "sport = :$1" 2>/dev/null | awk '{print $4}' | head -1
}

# --log_info alone enables only LOG_INFO (not "up to"), so --log_warning is
# needed too for the remote-compile warning to reach the log.
start_orlyi() {
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=$PORT --slave_port_number=$SLAVE_PORT \
           --ws_port_number=$WS_PORT --reporting_port_number=$REPORTING_PORT \
           --connection_backlog=10 \
           --instance_name=orly_bind_defaults_smoke \
           --starting_state=SOLO \
           --package_dir="$WORK/packages" \
           --le --log_info --log_warning "$@" \
           > "$WORK/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 60); do
    if [ -n "$(bound $WS_PORT)" ] && [ -n "$(bound $SLAVE_PORT)" ]; then return; fi
    sleep 1
  done
  fail "orlyi failed to open its listeners"
}

stop_orlyi() {
  kill -9 "$ORLYI_PID" 2>/dev/null || true
  wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
}

expect_bound() {  # port, expected address, listener name
  local got
  got="$(bound "$1")"
  echo "  $3 listener: $got"
  [ "$got" = "$2:$1" ] || fail "$3 listener bound to '$got', expected $2:$1"
  grep -q "$3 listener bound to $2:$1" "$WORK/orlyi.log" \
    || fail "the log does not say '$3 listener bound to $2:$1'"
}

echo "[1/2] defaults: client/ws/reporting on 127.0.0.1, replication on 0.0.0.0, compile refused"
start_orlyi
expect_bound $PORT 127.0.0.1 client
expect_bound $WS_PORT 127.0.0.1 websocket
expect_bound $REPORTING_PORT 127.0.0.1 reporting
expect_bound $SLAVE_PORT 0.0.0.0 replication
grep -q "remote compile disabled" "$WORK/orlyi.log" || fail "the log does not say remote compile is disabled"
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 - "$WS_PORT" "$OTHER_IP" <<'PY' || fail "the default-run checks failed"
import socket, sys
import orly
port, other = int(sys.argv[1]), sys.argv[2]
c = orly.connect(f"ws://127.0.0.1:{port}/", timeout=10, recv_timeout=60)
c.new_session()
try:
    c.send('compile "x = 42;";')
    raise SystemExit("compile was accepted without --allow_remote_compile")
except orly.RemoteCompileDisabled as err:
    assert err.reply["status"] == "remote_compile_disabled", err.reply
    print("  compile refused:", err.reply["result"])
assert c.send("echo 'still here';") == "still here"
if other:
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((other, port))
        raise SystemExit(f"the websocket port answered on {other}")
    except ConnectionRefusedError:
        print(f"  {other}:{port} refused")
else:
    print("  no non-loopback address on this host; skipped the refusal check")
PY
stop_orlyi

echo "[2/2] --bind_address=0.0.0.0 --allow_remote_compile: every interface, compile accepted"
start_orlyi --bind_address=0.0.0.0 --allow_remote_compile
expect_bound $PORT 0.0.0.0 client
expect_bound $WS_PORT 0.0.0.0 websocket
expect_bound $REPORTING_PORT 0.0.0.0 reporting
expect_bound $SLAVE_PORT 0.0.0.0 replication
grep -q "accepts compile from any client" "$WORK/orlyi.log" || fail "no warning about remote compile on a public listener"
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 - "$WS_PORT" "$OTHER_IP" <<'PY' || fail "the --bind_address=0.0.0.0 checks failed"
import sys
import orly
port, other = int(sys.argv[1]), sys.argv[2]
c = orly.connect(f"ws://{other or '127.0.0.1'}:{port}/", timeout=10, recv_timeout=60)
c.new_session()
print(f"  connected on {other or '127.0.0.1'}:{port}")
# Source that cannot compile: the reply is a compile error, not a refusal.
try:
    c.send('compile "this is not orlyscript";')
    raise SystemExit("a bad source compiled")
except orly.RemoteCompileDisabled:
    raise SystemExit("compile refused with --allow_remote_compile")
except orly.OrlyError as err:
    print("  compile accepted (source rejected):", err.reply["status"])
assert c.send("echo 'still here';") == "still here"
PY
kill -0 "$ORLYI_PID" 2>/dev/null || fail "orlyi died"
stop_orlyi
echo "BIND DEFAULTS OK"
