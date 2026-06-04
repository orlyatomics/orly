#!/bin/bash
# End-to-end driver for the agent-swarm knowledge-graph demo.
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

pkill -9 -f 'instance_name=agent_swarm_demo' 2>/dev/null || true
sleep 1

echo "[3/5] start fresh orlyi"
# Conservative resource caps mirroring wikipedia-pageviews/run.sh.
# The fiber pool needs headroom for per-layer Fiber::TSync parallelism
# during reads under many concurrent agents.
"$ORLYI" --mem_sim --create=true \
         --port_number=19400 --slave_port_number=19401 \
         --connection_backlog=10 \
         --instance_name=agent_swarm_demo \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --max_parallel_frames 4000 \
         --page_cache_size 256 \
         --block_cache_size 64 \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[4/5] wait for WebSocket port 8082"
for _ in $(seq 1 60); do
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

echo "[5/5] run demo.py"
if ! python3 demo.py; then
  echo ""
  echo "demo.py failed; dumping orlyi.log for diagnosis:"
  echo "=========================================================="
  cat "$WORK/orlyi.log"
  echo "=========================================================="
  exit 1
fi
