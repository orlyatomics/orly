#!/bin/bash
# End-to-end restart-durability test (#435): verify that a disk-backed orlyi
# gives data back after a stop/restart, and that the installed-package
# registry follows suit.
#
#   cycle 1: create volume, install kv, write keys, stop
#   cycle 2: restart --create=false; package must auto-reinstall and the
#            data must read back with NO client install; then uninstall, stop
#   cycle 3: restart; package must STAY uninstalled (clean error)
#
# Needs root for losetup and the /proc/partitions device scan; run under
# sudo or on a CI runner with passwordless sudo.  Ports 19600-19603.
set -e
cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
WORK="$(mktemp -d)"
LOOP=""
SRV_PID=""

cleanup() {
  [ -n "$SRV_PID" ] && sudo kill -9 "$SRV_PID" 2>/dev/null || true
  [ -n "$LOOP" ] && sudo losetup -d "$LOOP" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "[1/8] compile kv package"
cat > "$WORK/kv.orly" <<'ORLY'
package #1;
read_val = (*<['values', n]>::(int?)) where { n = given::(int); };
write_val = ((true) effecting { new <['values', n]> <- x; } ) where {
  n = given::(int);
  x = given::(int);
};
ORLY
"$ORLY_OUT/orly/orlyc" --skip-tests -o "$WORK" "$WORK/kv.orly"
mkdir "$WORK/packages" && touch "$WORK/packages/__orly__" && cp "$WORK/kv.1.so" "$WORK/packages/"

echo "[2/8] create loopback volume"
truncate -s 3G "$WORK/disk.img"
LOOP="$(sudo losetup -fP --show "$WORK/disk.img")"
sudo "$ORLY_OUT/orly/indy/disk/util/orly_dm" --create-volume --device-speed=fast \
     --instance-name=restart_test --num-devices=1 "$(basename "$LOOP")"

start_server() {  # $1 = create true|false, $2 = log tag
  sudo "$ORLY_OUT/orly/server/orlyi" \
    --create="$1" --instance_name=restart_test --starting_state=SOLO \
    --port_number=19600 --slave_port_number=19601 --ws_port_number=19602 \
    --reporting_port_number=19603 --connection_backlog=10 \
    --package_dir="$WORK/packages" --max_parallel_frames=4000 \
    --page_cache_size=256 --block_cache_size=64 --do_fsync --no_realtime \
    > "$WORK/orlyi-$2.log" 2>&1 &
  SRV_PID=$!
  for _ in $(seq 1 60); do
    ss -tln 2>/dev/null | grep -q ':19600' && return 0
    if ! sudo kill -0 "$SRV_PID" 2>/dev/null; then
      echo "orlyi ($2) died during startup:"; tail -20 "$WORK/orlyi-$2.log"; exit 1
    fi
    sleep 5
  done
  echo "orlyi ($2) never came up:"; tail -20 "$WORK/orlyi-$2.log"; exit 1
}

stop_server() {
  # SIGINT triggers TServer::Shutdown() (flush + orderly stop, #440);
  # wait for the graceful exit, with kill -9 only as a last resort.
  sudo kill -INT "$SRV_PID" 2>/dev/null || true
  for _ in $(seq 1 30); do
    sudo kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 2
  done
  sudo kill -9 "$SRV_PID" 2>/dev/null || true
  SRV_PID=""
  sleep 2
}

client() { PYTHONPATH="$REPO_ROOT/clients/python" python3 -c "$1"; }

echo "[3/8] start fresh (create=true), install, write"
start_server true run1
# The pov id is captured for cycle 2: a pre-restart pov must be REFUSED
# after the restart, not silently resurrected as an empty shell (#439).
OLD_POV="$(client "
import orly
c = orly.connect('ws://127.0.0.1:19602/', timeout=10, recv_timeout=60)
c.new_session(); c.install('kv', 1); pov = c.new_pov()
for n in range(1, 11):
    c.call(pov, 'kv', 'write_val', {'n': n, 'x': n * 100})
assert c.call(pov, 'kv', 'read_val', {'n': 5}) == 500
print(pov)
c.close()" | tail -1)"
echo "   wrote 10 keys via pov $OLD_POV"

echo "[4/8] stop (flush-on-shutdown makes the old 75s flush window unnecessary, #440)"
stop_server

echo "[5/8] restart (create=false): data + package must survive; old pov must be refused"
start_server false run2
client "
import orly
c = orly.connect('ws://127.0.0.1:19602/', timeout=10, recv_timeout=60)
c.new_session(); pov = c.new_pov()
vals = [c.call(pov, 'kv', 'read_val', {'n': n}) for n in range(1, 11)]
assert vals == [n * 100 for n in range(1, 11)], f'data lost: {vals}'
print('   data + auto-reinstalled package OK:', vals)
# Povs are ephemeral (#439): the pre-restart pov's durable record reloads,
# but its un-promoted state is gone -- the server must say so instead of
# minting an empty shell that reads through to global.
try:
    c.call('$OLD_POV', 'kv', 'read_val', {'n': 5})
    raise SystemExit('pre-restart pov was resurrected silently (#439)')
except orly.OrlyError as ex:
    assert 'ephemeral' in str(ex), f'wrong error for dead pov: {ex}'
print('   pre-restart pov refused cleanly (#439)')
c.uninstall('kv', 1)
c.close()"

echo "[6/8] stop"
stop_server

echo "[7/8] restart: uninstall must have survived"
start_server false run3
client "
import orly
c = orly.connect('ws://127.0.0.1:19602/', timeout=10, recv_timeout=60)
c.new_session(); pov = c.new_pov()
try:
    c.call(pov, 'kv', 'read_val', {'n': 1})
    raise SystemExit('package resurrected after uninstall+restart')
except orly.OrlyError as ex:
    assert 'non-installed' in str(ex), str(ex)
print('   uninstall persisted OK')
c.close()"
stop_server

echo "[8/8] PASS: restart durability verified"
