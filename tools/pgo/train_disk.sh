#!/bin/bash
# Disk-mode training run for tools/pgo/pgo_build.sh: real-disk ingest, disk merges, the WAL, a clean stop
# and a reopen, against an instrumented orlyi on a loop device.
#
#   tools/pgo/train_disk.sh [ORLY_OUT]     (default ../out_orly/release)
#
# The smokes run against the mem-sim orlyi, which never touches the disk controller, the WAL or the
# real-disk merge code. Under a write-heavy load those paths decide whether merges keep up with ingest, so a
# profile without them leaves the merge path compiled as if it were rarely run. This script trains them:
#   1. makes a sparse file, attaches it as a loop device (sudo losetup, as the disk tests in tests/ do),
#      gives the device to the current user and creates an orly volume on it with orly_dm, and gives the
#      instrumented orlyi cap_sys_admin and cap_sys_nice (sudo setcap), without which orlyi cannot open a
#      real volume as a normal user ("TServer failed to initialize: Permission denied"); the profile-guided
#      rebuild that follows replaces the binary, so the final orlyi carries no capabilities;
#   2. starts orlyi on that volume and runs layer_count.mjs (K writers of BATCH-key transactions plus a
#      reader) for SECS seconds, then pool_pressure.mjs, so data keeps flushing to disk and merging; then
#      ws_scale.mjs and read_fanout.mjs, so the reads that go to the volume (the block and page caches, the
#      on-disk file readers) are trained too, not only the mem-sim versions of them the smokes cover;
#   3. stops orlyi with SIGTERM (an instrumented binary writes its profile only on a clean exit), reopens
#      the volume, runs a short layer_count against what is there, and stops it again.
# Data is synthetic (the sample and ws_scale packages' generated keys and values). Needs passwordless sudo for losetup and setcap,
# node and npm, and orlyi, orlyc and orly_dm built under ORLY_OUT.
#
# Environment: SECS (default 150), K (16), BATCH (32), VOLUME_GB (16), PORT (19760; uses PORT..PORT+3),
# WORK (a temporary directory, removed on exit; it holds the sparse image, so put it on a disk with room for
# what the run writes, a few GiB).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ORLY_OUT="$(cd "${1:-$ROOT/../out_orly/release}" && pwd)"
ORLYI="$ORLY_OUT/orly/server/orlyi"
ORLYC="$ORLY_OUT/orly/orlyc"
ORLY_DM="$ORLY_OUT/orly/indy/disk/util/orly_dm"
SECS="${SECS:-150}"
K="${K:-16}"
BATCH="${BATCH:-32}"
VOLUME_GB="${VOLUME_GB:-16}"
PORT="${PORT:-19760}"
INSTANCE=orly_pgo_disk_train

for bin in "$ORLYI" "$ORLYC" "$ORLY_DM"; do
  [ -x "$bin" ] || { echo "missing: $bin" >&2; exit 1; }
done
sudo -n true 2>/dev/null || { echo "the disk training needs passwordless sudo (for losetup and setcap); see docs/pgo.md" >&2; exit 1; }
sudo setcap cap_sys_admin,cap_sys_nice=ep "$ORLYI"

WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
LOOP=""
PID=""
cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill -9 "$PID" 2>/dev/null || true; fi
  [ -z "$LOOP" ] || sudo losetup -d "$LOOP" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "-- disk: volume of ${VOLUME_GB} GiB (sparse) in $WORK"
truncate -s "${VOLUME_GB}G" "$WORK/disk.img"
LOOP="$(sudo losetup --show -f "$WORK/disk.img")"
sudo chown "$(id -u)" "$LOOP"
DEV="$(basename "$LOOP")"
"$ORLY_DM" --scan="$DEV" --create-volume --device-speed=fast --instance-name="$INSTANCE" --num-devices=1 "$DEV" > "$WORK/orly_dm.log" 2>&1 \
  || { cat "$WORK/orly_dm.log" >&2; exit 1; }

mkdir -p "$WORK/packages"
touch "$WORK/packages/__orly__"
for src in "$ROOT/clients/mcp/smoke/sample.orly" "$ROOT/clients/smoke/ws_scale.orly"; do
  (cd "$WORK" && "$ORLYC" -o "$WORK" "$src") > /dev/null
done
cp "$WORK"/sample.1.so "$WORK"/ws_scale.1.so "$WORK/packages/"

start() {  # $1 = true to create the store, false to open it
  "$ORLYI" --create="$1" --device="$DEV" --instance_name="$INSTANCE" --starting_state=SOLO \
           --port_number="$PORT" --replica_port_number=$((PORT + 1)) \
           --ws_port_number=$((PORT + 2)) --reporting_port_number=$((PORT + 3)) \
           --connection_backlog=64 --package_dir="$WORK/packages" --do_fsync --no_realtime \
           --page_cache_size=1024 --block_cache_size=1024 \
           >> "$WORK/orlyi.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 600); do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    if (echo > "/dev/tcp/127.0.0.1/$((PORT + 2))") 2>/dev/null; then return 0; fi
    sleep 0.2
  done
  echo "orlyi did not come up; last log lines:" >&2
  tail -40 "$WORK/orlyi.log" >&2
  exit 1
}

stop() {  # SIGTERM and wait: the profile is written at exit
  kill -TERM "$PID"
  for _ in $(seq 1 1200); do
    kill -0 "$PID" 2>/dev/null || { wait "$PID" 2>/dev/null || true; PID=""; return 0; }
    sleep 0.25
  done
  echo "orlyi did not stop within 300 s; killing it (its profile is lost)" >&2
  kill -9 "$PID" 2>/dev/null || true
  PID=""
}

run() {  # $1 = smoke script, rest = its environment
  local script="$1"; shift
  (cd "$ROOT/clients/smoke" && env ORLY_URL="ws://127.0.0.1:$((PORT + 2))/" ORLY_REPORT_PORT=$((PORT + 3)) "$@" node "$script") \
    || echo "-- disk: $script exited $? (the profile keeps what ran)"
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "orlyi exited during $script; last log lines:" >&2
    tail -40 "$WORK/orlyi.log" >&2
    exit 1
  fi
}

start true
echo "-- disk: ingest, $K writers x $BATCH keys per transaction plus a reader, ${SECS} s"
run layer_count.mjs K="$K" BATCH="$BATCH" SECS="$SECS" MAX_WRITES=100000000 QUIET_SECS=10
echo "-- disk: write-pool pressure"
run pool_pressure.mjs K=8 SECS=30 MAX_WRITES=200000
echo "-- disk: point and prefix reads from the volume, 1 to 64 sessions"
run ws_scale.mjs LEVELS=1,16,64 SECS=10
echo "-- disk: many readers over disk layers"
run read_fanout.mjs LAYERS=4 K=8 R=64 SECS=30 LOAD_SECS=60
stop
echo "-- disk: reopen, short ingest, stop"
start false
run layer_count.mjs K=8 BATCH="$BATCH" SECS=20 MAX_WRITES=100000000 QUIET_SECS=5
stop
echo "-- disk: done"
