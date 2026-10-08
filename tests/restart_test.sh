#!/bin/bash
# End-to-end restart-durability test (#435): verify that a disk-backed orlyi
# gives data back after a stop/restart, and that the installed-package
# registry follows suit.
#
#   cycle 1: create volume, install kv, write keys, stop
#   cycle 2: restart --create=false; package must auto-reinstall and the
#            data must read back with NO client install; then upgrade to
#            kv.2, check that uninstalling kv.1 is refused (#800), uninstall
#            kv.2, stop
#   cycle 3: restart; package must STAY uninstalled (clean error), neither
#            version coming back (#800)
#
# Needs root for losetup and the /proc/partitions device scan; run under
# sudo or on a CI runner with passwordless sudo.  Ports 19600-19603.
set -e
cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
ORLY_OUT="${ORLY_OUT:-$REPO_ROOT/../out_orly/debug}"
INSTANCE="rt435_$(python3 -c 'import uuid; print(uuid.uuid4().hex[:16])')"
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
# Version 2, the same package, for the uninstall-version checks in cycle 2 (#800).
mkdir "$WORK/v2"
sed 's/^package #1;/package #2;/' "$WORK/kv.orly" > "$WORK/v2/kv.orly"
"$ORLY_OUT/orly/orlyc" --skip-tests -o "$WORK/v2" "$WORK/v2/kv.orly"
cp "$WORK/v2/kv.2.so" "$WORK/packages/"

echo "[2/8] create loopback volume"
echo "   instance: $INSTANCE"
truncate -s 3G "$WORK/disk.img"
LOOP="$(sudo losetup -fP --show "$WORK/disk.img")"
sudo "$ORLY_OUT/orly/indy/disk/util/orly_dm" --create-volume --device-speed=fast \
     --instance-name="$INSTANCE" --num-devices=1 "$(basename "$LOOP")"

start_server() {  # $1 = create true|false, $2 = log tag
  sudo "$ORLY_OUT/orly/server/orlyi" \
    --create="$1" --instance_name="$INSTANCE" --starting_state=SOLO \
    --port_number=19600 --slave_port_number=19601 --ws_port_number=19602 \
    --reporting_port_number=19603 --connection_backlog=10 \
    --package_dir="$WORK/packages" --max_parallel_frames=4000 \
    --page_cache_size=256 --block_cache_size=64 --do_fsync --no_realtime \
    --log_info \
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

stop_server() {  # $1 = signal (INT or TERM), $2 = log tag
  # SIGINT and SIGTERM both trigger TServer::Shutdown() (flush + orderly stop,
  # #440; SIGTERM is what docker stop sends, #598). The shutdown must finish on
  # its own: needing kill -9 fails the test, since the data checks after a
  # restart can't tell a flushed shutdown from a lucky one.
  sudo kill "-$1" "$SRV_PID" 2>/dev/null || true
  for _ in $(seq 1 30); do
    sudo kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 2
  done
  if sudo kill -0 "$SRV_PID" 2>/dev/null; then
    sudo kill -9 "$SRV_PID" 2>/dev/null || true
    echo "orlyi ($2) ignored SIG$1 for 60s:"; tail -20 "$WORK/orlyi-$2.log"; exit 1
  fi
  if ! grep -q "TServer::Shutdown() complete" "$WORK/orlyi-$2.log"; then
    echo "orlyi ($2) exited on SIG$1 without completing its shutdown:"; tail -20 "$WORK/orlyi-$2.log"; exit 1
  fi
  # A completed Shutdown() is not a clean exit: the server is destroyed after it, and on a disk
  # volume that once aborted every time, after "Shutdown() complete" (#648). sudo exits with the
  # server's status, or 128+N if a signal killed it.
  local rc=0
  wait "$SRV_PID" || rc=$?
  SRV_PID=""
  if [ "$rc" -ne 0 ] || grep -qE "FATAL ERROR|TERMINATE" "$WORK/orlyi-$2.log"; then
    echo "orlyi ($2) did not exit cleanly on SIG$1 (exit $rc):"; tail -20 "$WORK/orlyi-$2.log"; exit 1
  fi
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

echo "[4/8] stop with SIGTERM, as docker stop does (#598; flush-on-shutdown, #440)"
stop_server TERM run1

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
# Nor may a new pov be made under it (#671): its repo is gone, and the child used to get an
# empty stand-in as its parent, with no way through to global, so it read nothing.
try:
    child = c.new_pov(parent='$OLD_POV')
    got = c.call(child, 'kv', 'read_val', {'n': 5})
    raise SystemExit(f'pov made under a dead pov reads {got!r} for a key global has as 500 (#671)')
except orly.OrlyError as ex:
    assert 'ephemeral' in str(ex), f'wrong error for a pov under a dead pov: {ex}'
print('   pov under the pre-restart pov refused cleanly (#671)')
# Uninstall names the installed version (#800): with kv.2 installed over kv.1, uninstalling
# kv.1 is refused and leaves kv.2 serving; and kv.1's record must not outlive the upgrade.
c.install('kv', 2)
try:
    c.uninstall('kv', 1)
    raise SystemExit('uninstall kv.1 succeeded with kv.2 installed (#800)')
except orly.OrlyError as ex:
    assert 'version 2 is the one installed' in str(ex), f'wrong error for a mismatched uninstall: {ex}'
assert c.call(pov, 'kv', 'read_val', {'n': 5}) == 500, 'kv.2 stopped serving after a refused uninstall'
print('   uninstall of a version that is not installed refused cleanly (#800)')
c.uninstall('kv', 2)
c.close()"
# The pre-restart pov's saved-repo entry in the system repo must go with the first repo creation
# after the restart; nothing removed one before #671, so the system repo grew without bound.
if ! grep -q "TManager: removed the saved entry of repo \[$OLD_POV\]" "$WORK/orlyi-run2.log"; then
  echo "pre-restart pov's saved-repo entry was not removed (#671)"; exit 1
fi
echo "   pre-restart pov's saved-repo entry removed (#671)"

echo "[6/8] stop"
stop_server INT run2

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
stop_server INT run3

echo "[8/8] PASS: restart durability verified"
