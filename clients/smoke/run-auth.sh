#!/bin/bash
# Shared-secret smoke (#710). orlyi takes an optional token (--auth_token_file,
# ORLY_AUTH_TOKEN_FILE, ORLY_AUTH_TOKEN or --auth_token); with one, every client
# connection must present it before any statement runs, and a replica must
# present the replication token (by default the same one) to join a primary.
# With none, nothing changes.
#   0. Build the TS client (clients/ts).
#   1. No token: WebSocket and binary clients work as before, with or without a
#      client token, and the startup log says nothing about tokens.
#   2. --auth_token_file: no token and a wrong one are refused with the typed
#      status over WebSocket (Python and TS clients) and the binary protocol
#      (raw handshakes, and orly_client through ORLY_AUTH_TOKEN), the right one
#      works, health checks need none, and no log line carries the token.
#   3. Control: the same refusal checks against an orlyi started WITHOUT the
#      token must fail, so step 2 can't pass against a server that ignores it.
#   4. --auth_token=<value>: the value is scrubbed from /proc/<pid>/cmdline.
#   5. Replication, primary started with --auth_token_file (the replication
#      token defaults to it): a replica with a different token is refused, with a
#      line in both logs, and exits; a replica with no token is refused; the
#      primary keeps listening, and a replica with the right token joins.
#   6. Control: a primary without a token lets the wrong-token replica join, so the
#      refusal in step 5 is the primary's check.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLY_CLIENT="$ORLY_OUT/orly/client/orly_client"
# Primary (or solo) ports, then the replica's.
PORT=19850 REPLICA_PORT=19851 WS_PORT=19852 REPORTING_PORT=19853
S_PORT=19860 S_REPLICA_PORT=19861 S_WS_PORT=19862 S_REPORTING_PORT=19863

for bin in "$ORLYI" "$ORLY_CLIENT"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

# Start from no token anywhere, whatever the caller's environment holds.
unset ORLY_AUTH_TOKEN ORLY_AUTH_TOKEN_FILE ORLY_REPLICATION_TOKEN ORLY_REPLICATION_TOKEN_FILE

echo "[0/6] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
PRIMARY_PID= REPLICA_PID=
trap 'for p in $PRIMARY_PID $REPLICA_PID; do kill -9 $p 2>/dev/null; done; rm -rf "$WORK"' EXIT
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"

# Fresh random tokens; files only the owner can read.
TOKEN="$(od -An -tx1 -N24 /dev/urandom | tr -d ' \n')"
OTHER="$(od -An -tx1 -N24 /dev/urandom | tr -d ' \n')"
umask 077
printf '%s\n' "$TOKEN" > "$WORK/token"
printf '%s\n' "$OTHER" > "$WORK/other"

fail() {
  echo "AUTH SMOKE FAIL: $*"
  for log in "$WORK"/*.log; do
    echo "--- $(basename "$log") (tail)"
    tail -15 "$log"
  done
  exit 1
}

LOG_FLAGS=(--le --log_info --log_notice --log_warning)

# start_primary <log name> [orlyi flags...]: a SOLO orlyi on the primary ports.
start_primary() {
  local log="$WORK/$1.log"; shift
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=$PORT --replica_port_number=$REPLICA_PORT \
           --ws_port_number=$WS_PORT --reporting_port_number=$REPORTING_PORT \
           --connection_backlog=10 --instance_name=orly_auth_smoke \
           --starting_state=SOLO --package_dir="$WORK/packages" \
           "${LOG_FLAGS[@]}" "$@" > "$log" 2>&1 &
  PRIMARY_PID=$!
  for _ in $(seq 1 60); do
    if ss -tln 2>/dev/null | grep -q ":$WS_PORT " && ss -tln 2>/dev/null | grep -q ":$REPLICA_PORT "; then return 0; fi
    kill -0 "$PRIMARY_PID" 2>/dev/null || fail "orlyi ($log) exited at startup"
    sleep 1
  done
  fail "orlyi ($log) failed to open its listeners"
}

# start_replica <log name> [orlyi flags...]: a REPLICA of the primary above.
start_replica() {
  local log="$WORK/$1.log"; shift
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=$S_PORT --replica_port_number=$S_REPLICA_PORT \
           --ws_port_number=$S_WS_PORT --reporting_port_number=$S_REPORTING_PORT \
           --connection_backlog=10 --instance_name=orly_auth_smoke_replica \
           --starting_state=REPLICA --address_of_primary=127.0.0.1:$REPLICA_PORT \
           --package_dir="$WORK/packages" \
           "${LOG_FLAGS[@]}" "$@" > "$log" 2>&1 &
  REPLICA_PID=$!
}

stop() {  # stop <pid variable name>
  local pid="${!1}"
  if [ -n "$pid" ]; then
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  eval "$1="
}

stop_all() {
  stop REPLICA_PID
  stop PRIMARY_PID
  for _ in $(seq 1 30); do
    if ! ss -tln 2>/dev/null | grep -qE ":($PORT|$REPLICA_PORT|$WS_PORT|$S_PORT|$S_WS_PORT) "; then return 0; fi
    sleep 1
  done
}

# wait_log <log name> <fixed string> <seconds>
wait_log() {
  for _ in $(seq 1 "$3"); do
    grep -qF -- "$2" "$WORK/$1.log" && return 0
    sleep 1
  done
  return 1
}

# No log may ever carry a token.
no_token_in_logs() {
  if grep -qF -e "$TOKEN" -e "$OTHER" "$WORK"/*.log; then
    fail "a log line carries a token"
  fi
}

check_clients() {  # check_clients <required|open>
  PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 auth_check.py "$1" $PORT $WS_PORT "$TOKEN"
}

echo "[1/6] no token: everything as before"
start_primary solo
check_clients open || fail "a client check failed with no token configured"
# Exercise both sides of the close-vs-token-write race (#775).
for attempt in $(seq 1 20); do
  out="$(echo "echo 'binary hello';" | ORLY_AUTH_TOKEN="$TOKEN" timeout -s KILL 30 "$ORLY_CLIENT" --server_address=127.0.0.1:$PORT 2>&1 || true)"
  echo "$out" | grep -qF "binary hello" || fail "orly_client with a token against a server without one (attempt $attempt): $out"
done
echo "  ok   orly_client: a client with a token still works (20 reconnects without one)"
grep -qF "token required" "$WORK/solo.log" && fail "the log mentions a token with none configured"
stop_all

echo "[2/6] --auth_token_file: refused without the token, served with it"
start_primary auth --auth_token_file="$WORK/token"
grep -qF "client listener bound to 127.0.0.1:$PORT; token required" "$WORK/auth.log" \
  || fail "the log does not say the client listener requires a token"
grep -qF "websocket listener bound to 127.0.0.1:$WS_PORT; remote compile disabled; token required" "$WORK/auth.log" \
  || fail "the log does not say the websocket listener requires a token"
check_clients required || fail "a client check failed with a token configured"
ORLY_URL="ws://127.0.0.1:$WS_PORT/" ORLY_SMOKE_TOKEN="$TOKEN" node auth.mjs || fail "the TS client checks failed"
# orlyi's own binary client, through the environment.
out="$(echo "echo 'binary hello';" | ORLY_AUTH_TOKEN="$TOKEN" timeout -s KILL 30 "$ORLY_CLIENT" --server_address=127.0.0.1:$PORT 2>&1 || true)"
echo "$out" | grep -qF "binary hello" || fail "orly_client with the token: $out"
echo "  ok   orly_client: right token (ORLY_AUTH_TOKEN) -> statements run"
out="$(echo "echo 'binary hello';" | ORLY_AUTH_TOKEN="$OTHER" timeout -s KILL 30 "$ORLY_CLIENT" --server_address=127.0.0.1:$PORT 2>&1 || true)"
echo "$out" | grep -qF "unauthorized" || fail "orly_client with a wrong token: $out"
echo "$out" | grep -qF "binary hello" && fail "orly_client ran a statement with a wrong token"
echo "  ok   orly_client: wrong token -> unauthorized"
echo "$out" | grep -F "unauthorized" | head -1 | sed 's/^/         /'
out="$(echo "echo 'binary hello';" | timeout -s KILL 30 "$ORLY_CLIENT" --server_address=127.0.0.1:$PORT 2>&1 || true)"
echo "$out" | grep -qF "unauthorized" || fail "orly_client with no token: $out"
echo "  ok   orly_client: no token -> unauthorized"
grep -qF "refused a client from" "$WORK/auth.log" || fail "the log has no refusal line"
kill -0 "$PRIMARY_PID" 2>/dev/null || fail "orlyi died"
no_token_in_logs
stop_all

echo "[3/6] control: the refusal checks must fail against a server that ignores the token"
start_primary control
if check_clients required > "$WORK/control.out" 2>&1; then
  cat "$WORK/control.out"
  fail "the refusal checks passed against an orlyi with no token; they prove nothing"
fi
grep -qF "a connection with no token was accepted" "$WORK/control.out" \
  || { cat "$WORK/control.out"; fail "the control failed, but not on the unauthenticated connection"; }
echo "  ok   the checks fail without a server-side token ($(grep -c FAIL "$WORK/control.out") failures)"
stop_all

echo "[4/6] --auth_token=<value> is scrubbed from the process's command line"
start_primary flag --auth_token="$TOKEN"
tr '\0' ' ' < "/proc/$PRIMARY_PID/cmdline" | grep -qF "$TOKEN" && fail "the token is still in /proc/<pid>/cmdline"
echo "  ok   /proc/<pid>/cmdline no longer carries the token"
PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" ORLY_AUTH_TOKEN="$TOKEN" python3 - $WS_PORT <<'PY' || fail "ORLY_AUTH_TOKEN did not authenticate the Python client"
import sys, orly
c = orly.connect(f"ws://127.0.0.1:{sys.argv[1]}/", retries=0)
assert c.new_session()
print("  ok   Python client: token from ORLY_AUTH_TOKEN")
PY
stop_all

echo "[5/6] replication: the primary refuses a replica without its token"
start_primary primary --auth_token_file="$WORK/token"
grep -qF "replication listener bound to 0.0.0.0:$REPLICA_PORT; token required" "$WORK/primary.log" \
  || fail "the replication token did not default to the client token"
start_replica wrong_replica --replication_token_file="$WORK/other"
wait_log primary "refused a replica from 127.0.0.1" 30 || fail "the primary did not refuse the wrong-token replica"
grep -qF "the replica presented a different replication token" "$WORK/primary.log" || fail "the primary's refusal doesn't say why"
wait_log wrong_replica "the primary refused this replica's replication token" 30 || fail "the replica did not log the refusal"
for _ in $(seq 1 30); do kill -0 "$REPLICA_PID" 2>/dev/null || break; sleep 1; done
kill -0 "$REPLICA_PID" 2>/dev/null && fail "the refused replica is still running"
echo "  ok   wrong token: refused, logged on both sides, the replica exited"
grep -h "replication: " "$WORK/primary.log" "$WORK/wrong_replica.log" | sed 's/^/         /'
stop REPLICA_PID
start_replica bare_replica
for _ in $(seq 1 30); do
  [ "$(grep -c "refused a replica from" "$WORK/primary.log")" -ge 2 ] && break
  sleep 1
done
[ "$(grep -c "refused a replica from" "$WORK/primary.log")" -ge 2 ] || fail "the primary did not refuse a replica with no token"
grep -E "refused a replica from .*(did not answer|hung up)" "$WORK/primary.log" > /dev/null \
  || fail "the primary's refusal of a tokenless replica doesn't say it had no token"
grep -q "to \[Replica\]" "$WORK/bare_replica.log" && fail "a replica with no token joined"
echo "  ok   no token: refused by the primary"
grep -h "refused a replica from" "$WORK/primary.log" | tail -1 | sed 's/^/         /'
stop REPLICA_PID
ORLY_REPLICATION_TOKEN="$TOKEN" start_replica right_replica
wait_log right_replica "to [Replica]" 120 || fail "the replica with the right token did not join"
grep -qF "accepted this replica's replication token" "$WORK/right_replica.log" || fail "the replica did not log the acceptance"
grep -qF "presented the replication token" "$WORK/primary.log" || fail "the primary did not log the acceptance"
kill -0 "$PRIMARY_PID" 2>/dev/null || fail "the primary died"
echo "  ok   right token: the replica joined"
no_token_in_logs
stop_all

echo "[6/6] control: a primary without a token lets the same wrong-token replica join"
start_primary open_primary
start_replica open_replica --replication_token_file="$WORK/other"
wait_log open_replica "to [Replica]" 120 || fail "the replica did not join a primary with no token"
grep -qF "did not ask for a replication token" "$WORK/open_replica.log" || fail "the replica did not log that the primary asked for no token"
echo "  ok   the wrong-token replica joins a tokenless primary, so step 5's refusal is the primary's"
no_token_in_logs
stop_all

echo "AUTH SMOKE OK"
