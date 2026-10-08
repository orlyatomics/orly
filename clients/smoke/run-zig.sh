#!/bin/bash
# Zig client smoke (#741): the Zig client (clients/zig) against a live orlyi.
#   0. Build the smoke driver (zig build smoke, in clients/zig). Set ZIG_SMOKE
#      to a prebuilt driver to skip this (a cross-compiled one, say), and ZIG
#      to name the zig binary.
#   1. Compile multi.orly with orlyc and start a fresh mem-sim orlyi.
#   2. smoke matrix: every new-POV flavour (safe/fast x shared/private) with
#      and without a parent (#580), pause/unpause, a mixed batch (#255), a
#      failing batch that leaves nothing behind, the same-method batch (#253),
#      and escaped strings.
#   3. Restart orlyi with --auth_token_file (#710). smoke auth: no token and a
#      wrong one are refused with the typed error and the raw reply kept as
#      detail, the right one works, and a remote compile is refused with its
#      own error. smoke token-env: the token from ORLY_AUTH_TOKEN_FILE, then
#      from ORLY_AUTH_TOKEN.
#   4. Control: the same auth checks against an orlyi started WITHOUT a token
#      must fail, so step 3 can't pass against a server that ignores the token.
#   5. Kill orlyi on exit.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
ZIG="${ZIG:-zig}"
PORT_BASE="${PORT_BASE:-19900}"
WS_PORT=$((PORT_BASE + 2))
URL="ws://127.0.0.1:$WS_PORT/"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

# Start from no token anywhere, whatever the caller's environment holds.
unset ORLY_AUTH_TOKEN ORLY_AUTH_TOKEN_FILE

if [ -z "$ZIG_SMOKE" ]; then
  echo "[0/4] build the Zig smoke driver"
  (cd "$REPO_ROOT/clients/zig" && "$ZIG" build smoke)
  ZIG_SMOKE="$REPO_ROOT/clients/zig/zig-out/bin/smoke"
fi

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

fail() {
  echo "ZIG SMOKE FAIL: $*"
  echo "orlyi log tail:"
  tail -20 "$WORK"/orlyi.log 2>/dev/null
  exit 1
}

# up <flags...>: a fresh mem-sim orlyi with multi.1.so installed.
up() {
  rm -rf "$WORK/packages"
  mkdir "$WORK/packages"
  touch "$WORK/packages/__orly__"
  cp "$WORK/multi.1.so" "$WORK/packages/"
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
           --ws_port_number=$WS_PORT --reporting_port_number=$((PORT_BASE + 3)) \
           --connection_backlog=10 \
           --instance_name=orly_zig_smoke \
           --starting_state=SOLO \
           --package_dir="$WORK/packages" "$@" \
           > "$WORK/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 60); do
    if (exec 3<>/dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null; then return 0; fi
    kill -0 "$ORLYI_PID" 2>/dev/null || fail "orlyi exited at startup"
    sleep 1
  done
  fail "orlyi did not open the WebSocket port $WS_PORT"
}

down() {
  kill -9 "$ORLYI_PID" 2>/dev/null || true
  wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
  for _ in $(seq 1 30); do
    if ! (exec 3<>/dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null; then return 0; fi
    sleep 1
  done
}

echo "[1/4] compile multi.orly and start orlyi (log -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/multi.orly")
up

echo "[2/4] new-POV matrix, pause/unpause, batches"
"$ZIG_SMOKE" matrix "$URL" || fail "the matrix checks failed"
down

echo "[3/4] auth: refusals and the token from the environment"
TOKEN="$(od -An -tx1 -N24 /dev/urandom | tr -d ' \n')"
umask 077
printf '%s\n' "$TOKEN" > "$WORK/token"
up --auth_token_file="$WORK/token"
"$ZIG_SMOKE" auth "$URL" "$TOKEN" || fail "the auth checks failed"
ORLY_AUTH_TOKEN_FILE="$WORK/token" "$ZIG_SMOKE" token-env "$URL" || fail "ORLY_AUTH_TOKEN_FILE did not authenticate"
ORLY_AUTH_TOKEN="$TOKEN" "$ZIG_SMOKE" token-env "$URL" || fail "ORLY_AUTH_TOKEN did not authenticate"
if grep -qF "$TOKEN" "$WORK/orlyi.log"; then fail "a log line carries the token"; fi
down

echo "[4/4] control: the refusal checks must fail against a server with no token"
up
if "$ZIG_SMOKE" auth "$URL" "$TOKEN" > "$WORK/control.out" 2>&1; then
  cat "$WORK/control.out"
  fail "the auth checks passed against an orlyi with no token; they prove nothing"
fi
grep -qF "a connection with no token was accepted" "$WORK/control.out" \
  || { cat "$WORK/control.out"; fail "the control failed, but not on the unauthenticated connection"; }
echo "  ok   the checks fail without a server-side token"

echo "ZIG SMOKE OK"
