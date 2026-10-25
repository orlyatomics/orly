#!/bin/bash
# `keys ... changed between (a, b)` smoke (#889).
#   1. Compile changed_between.orly with orlyc.
#   2. Start orlyi with --retention_versions, so the global POV keeps history, and run
#      changed_between.mjs: it writes edges, settles them on disk at two versions, and checks the
#      keys that changed between them, in either order, with a keyset bound, and that a version
#      past the newest and a private POV are refused.

set -e

cd "$(dirname "$0")"
SMOKE_DIR="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/release}"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
WS_PORT=19892

for bin in "$ORLYI" "$ORLYC"; do
  if [ ! -x "$bin" ]; then
    echo "missing: $bin"
    echo "Build the project first (from $REPO_ROOT):  make release"
    exit 1
  fi
done

echo "[0/2] build clients/ts"
(cd "$REPO_ROOT/clients/ts" && npm install --silent && npx tsc)

WORK="$(mktemp -d)"
ORLYI_PID=
trap 'test -n "$ORLYI_PID" && kill -9 $ORLYI_PID 2>/dev/null; rm -rf "$WORK"' EXIT

echo "[1/2] compile changed_between.orly"
(cd "$WORK" && "$ORLYC" -o "$WORK" "$SMOKE_DIR/changed_between.orly")
mkdir "$WORK/packages"
touch "$WORK/packages/__orly__"
cp "$WORK/changed_between.1.so" "$WORK/packages/"

echo "[2/2] start orlyi (logs -> $WORK/orlyi.log), run changed_between.mjs"
"$ORLYI" --mem_sim --mem_sim_mb=256 --mem_sim_slow_mb=64 --create=true \
         --port_number=19890 --replica_port_number=19891 \
         --ws_port_number=$WS_PORT --reporting_port_number=19893 \
         --connection_backlog=10 \
         --instance_name=orly_changed_between_smoke \
         --starting_state=SOLO \
         --package_dir="$WORK/packages" \
         --retention_versions=1000 \
         > "$WORK/orlyi.log" 2>&1 &
ORLYI_PID=$!
for _ in $(seq 1 60); do
  if ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then break; fi
  sleep 1
done
if ! ss -tln 2>/dev/null | grep -q ":$WS_PORT"; then
  echo "orlyi failed to come up; last log lines:"
  tail -20 "$WORK/orlyi.log"
  exit 1
fi

export ORLY_URL="ws://127.0.0.1:$WS_PORT/"
status=0
node changed_between.mjs || status=$?
if ! kill -0 "$ORLYI_PID" 2>/dev/null; then
  echo "CHANGED BETWEEN FAIL: orlyi died"
  status=1
fi
if [ "$status" -ne 0 ]; then
  echo "orlyi log tail:"
  tail -20 "$WORK/orlyi.log"
fi
exit "$status"
