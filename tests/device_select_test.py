#!/usr/bin/env python3
"""Telling orlyi and orly_dm which devices to use (#970).

A byte copy of a volume (a dd'd image on a RAM disk or a second loop device) has the same
superblock, so the same instance name and volume id, as the original. Steps:

1. Make a volume on a loop device, open it once with --create=true, and stop.
2. Copy its backing file and attach the copy to a second loop device.
3. orlyi --create=false with no --device must refuse to start and name both devices.
4. Two orlyi, one --device=<loop A> and one --device=<loop B>, must both open, in parallel,
   each using only its own device.
5. orly_dm --list --scan=<loop A> --scan=<loop B> must list both copies (it used to assert on two
   devices at one position), and orly_dm --list --scan=<loop A> only the first.
6. With --ram=/dev/ramN (a brd RAM disk, which has no queue/rq_affinity), a volume made on it
   must open. Off by default: it overwrites that device.

Needs root (losetup); run it through tests/device_select_test.sh.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pause_restart_test import Server  # noqa: E402

ASSIGNED = re.compile(r'Assigning Device \[(/dev/[^\]]+)\]')


def log(msg):
    print(f'[device select] {msg}', flush=True)


def orly_dm(args, *flags):
    return subprocess.run([os.path.join(args.orly_out, 'orly/indy/disk/util/orly_dm'), *flags],
                          capture_output=True, text=True)


def server(args, work, name, port, devices):
    a = argparse.Namespace(**vars(args))
    a.port = port
    a.extra_args = ' '.join([args.extra_args] + [f'--device={d}' for d in devices])
    return Server(a, work, name)


def run(args, work, violations):
    def require(cond, msg):
        if not cond:
            violations.append(msg)
            log(f'VIOLATION: {msg}')
        return cond

    # 1. The original.
    a = server(args, work, 'a', args.port, [])
    a.create_volume()
    loop_a = os.path.basename(a.loop)
    a.args.extra_args += f' --device={loop_a}'
    err = a.start(create=True)
    if not require(err is None, f'creating the store on {loop_a}: {err}\n{a.tail()}'):
        return
    a.stop()

    # 2. The copy, on a second loop device.
    b = server(args, work, 'b', args.port + 4, [])
    b.instance = a.instance
    shutil.copyfile(a.image, b.image)
    b.attach()
    loop_b = os.path.basename(b.loop)
    log(f'instance {a.instance} on {loop_a} and, copied, on {loop_b}')
    try:
        # 3. No --device: both copies are found, and that is an error naming both.
        both = server(args, work, 'both', args.port + 8, [])
        both.instance = a.instance
        err = both.start(create=False)
        both.stop()
        text = both.log_text()
        require(err is not None and 'exited' in err, f'orlyi with no --device started on two copies of one volume: {err}')
        require(f'/dev/{loop_a}' in text and f'/dev/{loop_b}' in text and 'copies of one volume' in text,
                'orlyi with no --device did not name both copies:\n' + both.tail())

        # 4. One copy each, in parallel.
        b.args.extra_args += f' --device=/dev/{loop_b}'
        err_a = a.start(create=False)
        err_b = b.start(create=False)
        require(err_a is None, f'orlyi --device={loop_a}: {err_a}\n{a.tail()}')
        require(err_b is None, f'orlyi --device=/dev/{loop_b}: {err_b}\n{b.tail()}')
        if err_a is None and err_b is None:
            require(a.proc.poll() is None and b.proc.poll() is None, 'both copies did not stay open together')
        a.stop()
        b.stop()
        used_a = set(ASSIGNED.findall(a.log_text()))
        used_b = set(ASSIGNED.findall(b.log_text()))
        require(used_a == {f'/dev/{loop_a}'}, f'orlyi --device={loop_a} used {sorted(used_a)}')
        require(used_b == {f'/dev/{loop_b}'}, f'orlyi --device=/dev/{loop_b} used {sorted(used_b)}')
        log(f'in parallel: {loop_a} used {sorted(used_a)}, {loop_b} used {sorted(used_b)}')

        # 5. orly_dm lists both copies, or the ones asked for. Scanned by name, not the whole host, so
        # that the test opens no volume it didn't make.
        listed = orly_dm(args, '--list', f'--scan={loop_a}', f'--scan=/dev/{loop_b}')
        require(listed.returncode == 0 and f'/dev/{loop_a} ' in listed.stdout and f'/dev/{loop_b} ' in listed.stdout,
                f'orly_dm --list ({listed.returncode}) did not list both copies:\n{listed.stdout}{listed.stderr}')
        scanned = orly_dm(args, '--list', f'--scan={loop_a}')
        require(scanned.returncode == 0 and f'/dev/{loop_a} ' in scanned.stdout and f'/dev/{loop_b} ' not in scanned.stdout,
                f'orly_dm --list --scan={loop_a} ({scanned.returncode}) listed:\n{scanned.stdout}{scanned.stderr}')
    finally:
        a.detach()
        b.detach()

    # 6. A device with no queue/rq_affinity.
    if args.ram:
        ram = os.path.basename(args.ram)
        missing = not os.path.exists(f'/sys/block/{ram}/queue/rq_affinity')
        log(f'{args.ram}: queue/rq_affinity {"missing" if missing else "present"}')
        r = server(args, work, 'ram', args.port + 12, [ram])
        made = orly_dm(args, '--create-volume', '--device-speed=fast', f'--instance-name={r.instance}',
                       '--num-devices=1', f'--scan={ram}', ram)
        if require(made.returncode == 0, f'orly_dm --create-volume on {args.ram} ({made.returncode}):\n{made.stderr[-2000:]}'):
            err = r.start(create=True)
            require(err is None, f'orlyi on {args.ram}: {err}\n{r.tail()}')
            r.stop()
            subprocess.call(['dd', 'if=/dev/zero', f'of={args.ram}', 'bs=1M', 'count=1', 'status=none'])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(os.path.dirname(__file__), '../../out_orly/debug')))
    ap.add_argument('--orlyi-prefix', default=os.environ.get('ORLYI_PREFIX', ''))
    ap.add_argument('--extra-args', default=os.environ.get('EXTRA_ARGS', ''))
    ap.add_argument('--port', type=int, default=19980, help='uses this port and the next 15')
    ap.add_argument('--volume-gb', type=int, default=2)
    ap.add_argument('--wal-mb', type=int, default=8)
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--ram', help='also make a volume on this brd RAM disk and open it (overwrites it)')
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('device_select_test.py needs root (losetup); run tests/device_select_test.sh')
    began = time.monotonic()
    work = tempfile.mkdtemp(prefix='devsel970.')
    violations = []
    try:
        os.makedirs(f'{work}/packages')
        open(f'{work}/packages/__orly__', 'w').close()
        run(args, work, violations)
    except Exception as ex:  # noqa: BLE001
        violations.append(f'{type(ex).__name__}: {ex}')
        log(f'VIOLATION: {violations[-1]}')
    finally:
        if not args.keep and not violations:
            shutil.rmtree(work, ignore_errors=True)
        else:
            log(f'work directory kept: {work}')
    log(f'DEVICE SELECT: violations={len(violations)} ({time.monotonic() - began:.0f}s)')
    sys.exit(1 if violations else 0)


if __name__ == '__main__':
    main()
