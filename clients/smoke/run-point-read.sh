#!/bin/bash
# Idle-server latency smoke (#764). On a fresh mem-sim orlyi holding one key,
# one session reads that key a few hundred times, one call at a time, and the
# smoke reports the read latency percentiles, how long orlyi took to start
# listening, and how much CPU it burns while idle. Prints METRIC lines for
# tools/maint/ab_bench.py.
#   0. Build the orly TS client (clients/ts).
#   1. Compile clients/smoke/read_fold.orly with orlyc; start a fresh orlyi.
#   2. Measure idle CPU for IDLE_SECS seconds.
#   3. Run point_read.mjs.
#   4. Check orlyi is still alive; kill it on exit.

set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19872
REPORT_PORT=19873
IDLE_SECS="${IDLE_SECS:-5}"

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

echo "[1/3] compile read_fold.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$REPO_ROOT/clients/smoke/read_fold.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/read_fold.1.so" "$WORK/packages/"

T0=$(date +%s.%N)
"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19870 --slave_port_number=19871 \
         --ws_port_number=$WS_PORT --reporting_port_number=$REPORT_PORT \
         --connection_backlog=32 \
         --instance_name=orly_point_read_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

for _ in $(seq 1 600); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then break; fi
  sleep 0.1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT "; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi
T1=$(date +%s.%N)
awk -v a="$T0" -v b="$T1" 'BEGIN { printf "METRIC startup_s %.3f\n", b - a }'

echo "[2/3] idle CPU over ${IDLE_SECS}s"
cpu_ticks() { awk '{ print $14 + $15 }' "/proc/$ORLYI_PID/stat"; }
sleep 2
C0=$(cpu_ticks)
sleep "$IDLE_SECS"
C1=$(cpu_ticks)
# Percent of one core.
awk -v a="$C0" -v b="$C1" -v hz="$(getconf CLK_TCK)" -v s="$IDLE_SECS" 'BEGIN { printf "METRIC idle_cpu_pct %.1f\n", (b - a) * 100 / hz / s }'

echo "[3/3] one session, one-key reads"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" node point_read.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || echo "orlyi exited with status $?"
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
