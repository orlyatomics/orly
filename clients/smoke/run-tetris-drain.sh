#!/bin/bash
# Tetris drain smoke (#801): eight paused POVs' backlogs are unpaused together and must all reach
# the global POV exactly, a POV paused mid-drain must hold still, guarded (`if`) writes must all
# survive their replay, and of eight competing claims on one key exactly one may be promoted.
# See tetris_drain.py for what each part pins and why.
#   1. Compile tetris_drain.orly with orlyc.
#   2. Start a fresh mem-sim orlyi on ports 19980-19983.
#   3. Run tetris_drain.py.
#   4. Fail unless it passed, orlyi is still alive, and its log shows no abort.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="${ORLYI:-$ORLY_OUT/orly/server/orlyi}"
ORLYC="${ORLYC:-$ORLY_OUT/orly/orlyc}"
PORT_BASE="${PORT_BASE:-19980}"
WS_PORT=$((PORT_BASE + 2))

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/3] compile tetris_drain.orly and start fresh orlyi (logs -> $WORK/orlyi.log)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/tetris_drain.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/tetris_drain.1.so" "$WORK/packages/"

"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=$PORT_BASE --slave_port_number=$((PORT_BASE + 1)) \
         --ws_port_number=$WS_PORT --reporting_port_number=$((PORT_BASE + 3)) \
         --connection_backlog=32 \
         --instance_name=orly_tetris_drain_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --update_pool_size=100000 --update_entry_pool_size=200000 \
         --log_info $EXTRA_ARGS \
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

echo "[3/3] drain, pause mid-drain, guarded writes, competing claims"
status=0
ORLY_URL="ws://127.0.0.1:$WS_PORT/" PYTHONPATH="$REPO_ROOT/clients/python" python3 tetris_drain.py > "$WORK/smoke.out" 2>&1 || status=$?
cat "$WORK/smoke.out"
ABORT='aborting|StepMergeDisk \[|StepMergeMem caught error|StepTail \[|Fiber Runner caught exception|FATAL|Tetris::TPlayer::Play error|could not tell session'
if grep -Eq "$ABORT" "$WORK/orlyi.log"; then
  echo "TETRIS DRAIN FAIL: orlyi logged an error:"
  grep -E "$ABORT" "$WORK/orlyi.log" | head -3
  status=1
fi
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  wait "$ORLYI_PID" || true
  echo "TETRIS DRAIN FAIL: orlyi died"
  status=1
fi
if ! grep -q "^TETRIS DRAIN OK" "$WORK/smoke.out"; then
  status=1
fi
if [ "$status" -ne 0 ]; then
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
