#!/bin/bash
# DIAGNOSTIC ONLY (#772): pool-pressure smoke with per-runner park/wake counters ($ORLY_DIAG772).
# Run as an A/A of one build: arm B (ORLY_OUT under /B/) polls with 10 us sleeps for
# B_SLEEP_PHASE_US (default 5000) before parking, arm A parks after the spin as on master.
set -e
case "$ORLY_OUT" in */B/*) export ORLY_RUNNER_SLEEP_PHASE_US="${B_SLEEP_PHASE_US:-5000}";; esac
export ORLY_DIAG772="$(mktemp -u /tmp/diag772.XXXXXX)"
LOG="$(mktemp)"
status=0
bash "${SMOKE:-clients/smoke/run-pool-pressure.sh}" > "$LOG" 2>&1 || status=$?
cat "$LOG"
echo "sleep phase: ${ORLY_RUNNER_SLEEP_PHASE_US:-0} us"
cat "$ORLY_DIAG772" || true
writes=$(grep -oE "^(shared POV|POV per writer): [0-9]+ writes" "$LOG" | grep -oE "[0-9]+" | awk '{s+=$1} END {print s+0}')
[ "$writes" -gt 0 ] || writes=$(grep -oE "^reads=[0-9]+" "$LOG" | grep -oE "[0-9]+" | head -1)
python3 - "$ORLY_DIAG772" "${writes:-0}" <<'PY' || true
import re, sys, collections
path, writes = sys.argv[1], int(sys.argv[2])
if not writes:
    sys.exit(0)
agg = collections.defaultdict(lambda: collections.Counter())
for line in open(path):
    label = line.split()[0]
    kv = dict(re.findall(r'(\w+)=(\S+)', line))
    c = agg[label]
    for k in ('frames', 'parks', 'wakes_in', 'sleeps', 'wake_out', 'wake_out_us'):
        c[k] += int(kv[k])
    gaps = [int(x) for x in kv['gaps'].split('/')]
    lat = [int(x) for x in kv['lat'].split('/')]
    lat_us = [int(x) for x in kv['lat_us'].split('/')]
    c['gaps_1to5ms'] += gaps[3]
    c['gaps_5to20ms'] += gaps[4]
    c['lat_n'] += sum(lat)
    c['lat_us'] += sum(lat_us)
tot = collections.Counter()
for c in agg.values():
    tot.update(c)
def m(name, v):
    print(f"METRIC {name} {v:.4f}")
m('wake_syscalls_per_write', tot['wake_out'] / writes)
m('wake_syscall_us_per_write', tot['wake_out_us'] / writes)
m('wake_syscall_us_mean', tot['wake_out_us'] / max(tot['wake_out'], 1))
m('wake_lat_us_mean', tot['lat_us'] / max(tot['lat_n'], 1))
m('parks_per_write', tot['parks'] / writes)
m('sleep_laps_per_write', tot['sleeps'] / writes)
for label, c in sorted(agg.items()):
    l = re.sub(r'\W', '_', label.strip('()'))
    if c['frames'] or c['wake_out']:
        m(f'frames_per_write__{l}', c['frames'] / writes)
        m(f'wakes_in_per_write__{l}', c['wakes_in'] / writes)
        m(f'wake_out_us_per_write__{l}', c['wake_out_us'] / writes)
        m(f'gaps_1to5ms__{l}', c['gaps_1to5ms'])
PY
exit $status
