#!/bin/bash
# orlyi under ThreadSanitizer (#713). The TSan job's unit tests never start a
# server, so the WebSocket sessions, Tetris, the merge runners and the
# reporting port had never run under TSan. This runs the memory-drain smoke
# (memory_drain.mjs, the #607 scenario: paused POVs filled to the reserve, then
# unpaused and drained) against an orlyi built with `jhm -c tsan`, then shuts
# it down with SIGINT, and counts the reports TSan wrote.
#
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/mcp/smoke/sample.orly with the TSan orlyc.
#   2. Negative control: the same run with ORLY_TSAN_RACY_CONTROL=1, which
#      makes orlyi race on purpose at startup (orlyi.cc). Fails unless that
#      race is counted, so a change that stops orlyi being instrumented, or
#      its reports being collected, can't pass step 3 by accident.
#   3. The gate: fails on any un-suppressed report, on a smoke failure, or if
#      orlyi dies or doesn't exit cleanly on SIGINT.
#
# Suppressions come from orly/tsan.supp, as for the unit tests. Every report
# is kept under $LOG_DIR (default ./tsan-logs/server).
#
# Run from anywhere, after `tools/jhm -c tsan orly/server/orlyi orly/orlyc`.
# TSan needs a low-ASLR address space: CI lowers vm.mmap_rnd_bits, and every
# binary here also runs under `setarch -R`.

set -u

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/tsan}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
LOG_DIR="$(mkdir -p "${LOG_DIR:-$PWD/tsan-logs/server}" && cd "${LOG_DIR:-$PWD/tsan-logs/server}" && pwd)"
SUPP="$REPO_ROOT/orly/tsan.supp"
WS_PORT=19762
REPORT_PORT=19763
# The scale #713 was found at. 32 POVs and 32 writers drain in seconds.
export POVS="${POVS:-32}" K="${K:-32}"
ARCH="$(uname -m)"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build it first (from $REPO_ROOT):  tools/jhm -c tsan orly/server/orlyi orly/orlyc"
    exit 1
  fi
done

echo "[0/3] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc) || exit 1

WORK="$(mktemp -d)"
ORLYI_PID=""
trap '[ -n "$ORLYI_PID" ] && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile sample.orly"
(cd "$WORK" && setarch "$ARCH" -R "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/mcp/smoke/sample.orly") || exit 1
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/sample.1.so" "$WORK/packages/"

# run_smoke <tag> [VAR=value...]: a fresh orlyi under TSan, the drain smoke
# against it, then SIGINT. Sets SMOKE_RC, ALIVE (yes/no), EXIT_RC and REPORTS.
run_smoke() {
  local tag=$1; shift
  local dir="$LOG_DIR/$tag"
  rm -rf "$dir" "$WORK/data"
  mkdir -p "$dir"
  env "$@" TSAN_OPTIONS="halt_on_error=0 exitcode=0 history_size=4 suppressions=$SUPP log_path=$dir/tsan" \
    setarch "$ARCH" -R "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=19760 --slave_port_number=19761 \
           --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
           --connection_backlog=32 \
           --instance_name=orly_tsan_server_smoke \
           --starting_state=SOLO \
           --package_dir="$WORK/packages" \
           --update_pool_size=5000 --update_entry_pool_size=10000 \
           > "$dir/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  local up=no
  for _ in $(seq 1 120); do
    if ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then up=yes; break; fi
    kill -0 $ORLYI_PID 2>/dev/null || break
    sleep 1
  done
  SMOKE_RC=1
  if [ "$up" = yes ]; then
    ORLY_URL="ws://127.0.0.1:$WS_PORT" ORLY_REPORT_PORT=$REPORT_PORT \
      timeout 300 node memory_drain.mjs > "$dir/smoke.log" 2>&1
    SMOKE_RC=$?
  else
    echo "orlyi didn't come up in 120 s" > "$dir/smoke.log"
  fi
  sed 's/^/    /' "$dir/smoke.log" | tail -5
  ALIVE=no
  kill -0 $ORLYI_PID 2>/dev/null && ALIVE=yes
  # SIGINT is orlyi's orderly shutdown (#440); SIGTERM is blocked.
  kill -INT $ORLYI_PID 2>/dev/null
  for _ in $(seq 1 120); do
    kill -0 $ORLYI_PID 2>/dev/null || break
    sleep 1
  done
  if kill -0 $ORLYI_PID 2>/dev/null; then
    echo "    orlyi didn't exit within 120 s of SIGINT; killing it"
    kill -9 $ORLYI_PID 2>/dev/null
  fi
  wait $ORLYI_PID 2>/dev/null
  EXIT_RC=$?
  ORLYI_PID=""
  REPORTS=$(cat "$dir"/tsan.* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
  echo "    smoke exit $SMOKE_RC, orlyi alive after smoke: $ALIVE, orlyi exit $EXIT_RC, TSan reports: $REPORTS"
}

echo "[2/3] negative control: orlyi with a deliberate race"
run_smoke control ORLY_TSAN_RACY_CONTROL=1
if [ "$SMOKE_RC" -ne 0 ] || [ "$REPORTS" -eq 0 ]; then
  echo "::error::TSan orlyi control: expected the smoke to pass and the deliberate race to be reported; got smoke exit $SMOKE_RC and $REPORTS report(s). The gate below would prove nothing."
  exit 1
fi
echo "TSan orlyi control OK: the deliberate race was reported ($REPORTS report(s))."

echo "[3/3] memory-drain smoke under TSan"
run_smoke smoke
fail=""
[ "$SMOKE_RC" -ne 0 ] && fail="$fail smoke-exit-$SMOKE_RC"
[ "$ALIVE" != yes ] && fail="$fail orlyi-died"
[ "$EXIT_RC" -ne 0 ] && fail="$fail orlyi-exit-$EXIT_RC"
if [ "$REPORTS" -ne 0 ]; then
  fail="$fail $REPORTS-tsan-report(s)"
  cat "$LOG_DIR"/smoke/tsan.* | grep -A12 "WARNING: ThreadSanitizer" | head -200
fi
if [ -n "$fail" ]; then
  echo "--- orlyi log tail"; tail -20 "$LOG_DIR/smoke/orlyi.log"
  echo "::error::orlyi under TSan:$fail. Reports are in the tsan-logs artifact (server/); if one is provably benign, document it and add a suppression to orly/tsan.supp."
  exit 1
fi
echo "No un-suppressed ThreadSanitizer reports from orlyi."
