#!/bin/bash
# Concurrent-write throughput benchmark for issue #234. Mirrors run-bench.sh but
# drives concurrent_bench.py (K writer threads -> one global POV). Deliberately
# NOT named run.sh / run-go.sh so the CI examples job (which invokes those by
# name) does not run it -- this is a manual benchmark, not a gated demo.
#
# Pass server flags through ORLYI_EXTRA_FLAGS, e.g. to exercise the #234
# commutative fast-lane once it lands:
#   ORLYI_EXTRA_FLAGS=--tetris_commutative_fastlane ./run-concurrent-bench.sh
set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    exit 1
  fi
done

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/5] compile graph.orly"
"$ORLYC" -o "$WORK" graph.orly

echo "[2/5] populate packages/"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/graph.1.so" "$WORK/packages/"

pkill -9 -f 'instance_name=agent_swarm_cbench' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi  (extra flags: ${ORLYI_EXTRA_FLAGS:-none})"
"$ORLYI" --mem_sim --create=true \
         --port_number=19410 --slave_port_number=19411 \
         --connection_backlog=64 \
         --instance_name=agent_swarm_cbench \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --max_parallel_frames 4000 \
         --page_cache_size 256 \
         --block_cache_size 64 \
         ${ORLYI_EXTRA_FLAGS:-} \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[4/5] wait for WebSocket port 8082 (debug + --mem_sim startup is ~75s)"
for _ in $(seq 1 150); do
  if ss -tln 2>/dev/null | grep -q ':8082'; then
    break
  fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ':8082'; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[5/5] run concurrent_bench.py"
if ! PYTHONPATH="$REPO_ROOT/clients/python:$PYTHONPATH" python3 concurrent_bench.py; then
  echo ""
  echo "concurrent_bench.py failed; dumping orlyi.log for diagnosis:"
  echo "=========================================================="
  cat "$WORK/orlyi.log"
  echo "=========================================================="
  exit 1
fi
