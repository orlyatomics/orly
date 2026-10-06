#!/bin/bash
# Release-build smoke for the aarch64 arm64-release CI job (#556).
#
# Both aarch64 bugs found so far reproduced ONLY in optimised builds, because
# each was the optimiser hoisting a thread-pointer read across a fiber switch
# (see tools/maint/lint_fiber_tls.py), and the arm64-build job builds debug:
#   #554  orlyc's embedded server wedged at teardown   -> step 1
#   #578  orlyi segfaulted right after the first write  -> step 2
#
#   1. Compile two packages with the release orlyc, each under a deadline:
#      one with test{} blocks, so orlyc stands up and tears down a whole
#      mem-sim server to run them, which is what hung (a package without tests
#      gets no server, #678); and the sample package step 2 installs.
#      On expiry: per-thread state + wchan, then fail.
#   2. RUNS times: start a fresh release orlyi, create every POV flavour,
#      write through each, and check the server is still alive. On a crash:
#      the faulting PC from the kernel log, symbolised with addr2line (release
#      is built -g1, so line tables exist), then fail.
#
# Usage: arm_release_smoke.sh [RUNS]   (ORLY_OUT defaults to ../out_orly/release)

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO_ROOT="$PWD"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
PKG="$REPO_ROOT/clients/mcp/smoke/sample.orly"
TESTED_PKG="$REPO_ROOT/tests/lang_tests/general/assertions.orly"
RUNS="${1:-10}"
WS_PORT=19722

for f in "$ORLYI" "$ORLYC" "$PKG" "$TESTED_PKG"; do
  test -e "$f" || { echo "missing: $f"; exit 1; }
done

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

# Ask the kernel to log the registers of a process killed by a fatal signal,
# so a crash in step 2 leaves its PC somewhere we can read. Best effort: it
# needs root, which CI runners have (via sudo) and a dev box may not.
if [ "$(id -u)" = 0 ]; then SUDO=; else SUDO="sudo -n"; fi
$SUDO sysctl -q kernel.print-fatal-signals=1 2>/dev/null || true

echo "[1/2] release orlyc on two packages (deadline 300s each)"
mkdir "$WORK/pkgout"
compile_under_deadline() {
  # `timeout` would need -s KILL anyway (RunUntilCtrlC masks SIGTERM), and a
  # killed process can't be interrogated, so poll instead.
  (cd "$WORK/pkgout" && exec "$ORLYC" -o "$WORK/pkgout" "$1") > "$WORK/orlyc.out" 2>&1 &
  PID=$!
  for _ in $(seq 1 60); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 5
  done
  if kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: orlyc still running after 300s on $1 (the #554 shape). Threads:"
    for t in /proc/$PID/task/*; do
      printf "  tid %s  state=%s  wchan=%s\n" "$(basename "$t")" \
        "$(awk '/^State:/{print $2}' "$t/status" 2>/dev/null)" \
        "$(cat "$t/wchan" 2>/dev/null)"
    done
    kill -9 "$PID"
    cat "$WORK/orlyc.out"
    exit 1
  fi
  if ! wait "$PID"; then
    echo "FAIL: orlyc exited non-zero on $1"
    cat "$WORK/orlyc.out"
    exit 1
  fi
}
compile_under_deadline "$TESTED_PKG"
compile_under_deadline "$PKG"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/pkgout/sample.1.so" "$WORK/packages/"
echo "  ok"

echo "[2/2] release orlyi: write through every POV flavour, $RUNS fresh servers"
for i in $(seq 1 "$RUNS"); do
  STARTED_AT="$(cut -d" " -f1 /proc/uptime)"
  "$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
           --port_number=19720 --slave_port_number=19721 \
           --ws_port_number=$WS_PORT --connection_backlog=10 \
           --instance_name=orly_arm_release_smoke --starting_state=SOLO \
           --package_dir="$WORK/packages" --le --log_info \
           > "$WORK/orlyi.log" 2>&1 &
  ORLYI_PID=$!
  for _ in $(seq 1 120); do
    (echo > /dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null && break
    kill -0 "$ORLYI_PID" 2>/dev/null || break
    sleep 0.5
  done

  # Raw statements rather than a driver's new_pov(), so this checks the
  # server and nothing else.
  set +e
  PYTHONPATH="$REPO_ROOT/clients/python" timeout 60 python3 - "$WS_PORT" <<'PY'
import sys
import orly
c = orly.connect(f"ws://127.0.0.1:{sys.argv[1]}/")
c.new_session()
c.install("sample", 1)
for n, kind in enumerate(["safe shared", "safe private", "fast shared", "fast private"]):
    pov = c.send(f"new {kind} pov;")
    c.call(pov, "sample", "write_val", {"n": n, "x": n + 10})
PY
  client_rc=$?
  set -e
  # #578's crash came just AFTER the write returned, so give it a moment.
  sleep 3

  if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
    set +e; wait "$ORLYI_PID"; rc=$?; set -e
    echo "FAIL: run $i: orlyi died (exit $rc) after writing (the #578 shape)"
    echo "--- orlyi log tail:"; tail -5 "$WORK/orlyi.log"
    echo "--- kernel report:"
    # Only what the kernel logged during this run, by its seconds-since-boot
    # stamp: the ring buffer holds every earlier crash too (and once full it
    # stops growing, so counting lines doesn't work), and inside a container
    # the pid it names is not ours.
    REPORT="$($SUDO dmesg 2>/dev/null \
              | awk -v t="$STARTED_AT" -F'[][]' '$2 + 0 >= t' \
              | grep -E 'unhandled exception|segfault|fatal signal' | head -3)"
    echo "${REPORT:-  (no access to the kernel log)}"
    # "... fault in orlyi[613ff4,aaaac9f90000+9b6000]": the first field is
    # the faulting offset into the binary.
    OFF="$(grep -oE ' in [^ []+\[[0-9a-f]+,' <<<"$REPORT" | head -1 | sed -E 's/.*\[([0-9a-f]+),/\1/' || true)"
    if [ -n "$OFF" ]; then
      echo "--- faulting PC orlyi+0x$OFF:"
      addr2line -f -i -C -e "$ORLYI" "0x$OFF" | sed 's/^/  /'
    fi
    ORLYI_PID=
    exit 1
  fi
  if [ $client_rc -ne 0 ]; then
    echo "FAIL: run $i: the client failed, but orlyi is still up"
    tail -5 "$WORK/orlyi.log"
    exit 1
  fi
  kill -9 "$ORLYI_PID"; wait "$ORLYI_PID" 2>/dev/null || true
  ORLYI_PID=
  echo "  run $i ok"
done
echo "arm release smoke: ok"
