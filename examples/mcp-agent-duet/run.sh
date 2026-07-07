#!/bin/bash
# End-to-end driver for the two-agent MCP demo (#528):
#   0. Build clients/ts + clients/mcp (the MCP server both agents spawn).
#   1. Compile ../agent-swarm/graph.orly (ONE schema source, reused verbatim).
#   2. Lay out a package dir, start a fresh mem-sim orlyi.
#   3. Run duet.mjs: mint one shared pov, spawn agents ada + byte concurrently
#      (each with its own orly-mcp stdio server), verify the merged graph
#      against corpus-derived ground truth. Non-zero exit on any mismatch.
set -e

cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19802

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make debug"
    exit 1
  fi
done

if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
  echo "missing: node/npm (install Node.js 18+)"
  exit 1
fi

echo "[0/4] build clients/ts and clients/mcp, install demo deps"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)
# npm copies file:../ts deps and won't refresh a stale same-version copy.
(cd "$REPO_ROOT/clients/mcp" && rm -rf node_modules/orly && npm install --silent && npx tsc)
npm install --silent

WORK="$(mktemp -d)"
trap 'kill -9 $ORLYI_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "[1/4] compile ../agent-swarm/graph.orly (shared schema)"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$OLDPWD/../agent-swarm/graph.orly")

echo "[2/4] populate packages/ and start fresh orlyi (logs -> $WORK/orlyi.log)"
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/graph.1.so" "$WORK/packages/"

pkill -9 -f 'instance_name=mcp_agent_duet' 2>/dev/null || true
sleep 1

"$ORLYI" --mem_sim --create=true \
         --port_number=19800 --slave_port_number=19801 \
         --ws_port_number=$WS_PORT \
         --connection_backlog=10 \
         --instance_name=mcp_agent_duet \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --max_parallel_frames 4000 \
         --page_cache_size 256 \
         --block_cache_size 64 \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!

echo "[3/4] wait for WebSocket port $WS_PORT"
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

echo "[4/4] run the duet"
if ! ORLY_URL="ws://127.0.0.1:$WS_PORT/" node duet.mjs; then
  echo ""
  echo "duet.mjs failed; dumping orlyi.log for diagnosis:"
  echo "=========================================================="
  cat "$WORK/orlyi.log"
  echo "=========================================================="
  exit 1
fi
