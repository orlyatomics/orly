#!/usr/bin/env python3
"""A paused safe POV stays paused across a restart (#961).

Pausing a safe POV writes a transaction record to the write-ahead log, and recovery replays it.
But the WAL kept nothing pinned for it: the POV's own Pov record (which says nothing about its
status) is copied forward once it is half the ring behind, and once no other pin held the head
below the Pause record, the head moved past it and a restart brought the POV back unpaused. Its
unpromoted updates were then promoted into its parent, though the client had paused it so that
they would not be.

Each scenario runs on a fresh loopback volume with a small WAL ring (--wal-mb, default 8):

  copied-empty    pause an empty POV, then write through another safe POV until the paused POV's
                  Pov record has been copied forward, SIGKILL, restart
  copied-writes   the same, with a few writes into the paused POV after the pause
  no-ring         pause an empty POV, write into it, SIGKILL and restart at once, with the Pause
                  record still in the ring (the replay of a pause with nothing pending)
  no-ring-empty   the same without the writes
  copied-held     copied-empty, with an older paused POV holding unpromoted writes, so the head
                  stays below the pause and recovery replays both the pause and its copy
  copied-unpaused copied-empty, unpaused before the SIGKILL: it must come back unpaused
  repo-copied     copied-writes, but write on until the paused POV itself (its updates and its
                  pause) has been copied forward twice (#963), so the records that wrote them have
                  left the ring and only the copy holds them
  repo-promoted   repo-copied, then unpaused and promoted before the SIGKILL: after the restart
                  the global POV reads exactly the POV's writes, once

After each restart it requires the POV to be paused (except copied-unpaused and repo-promoted),
its updates to be unpromoted (still pending, and not visible from a new POV of the global POV,
after a wait), and then that unpausing it promotes them.

Needs root (losetup), like tests/restart_test.sh; run it through tests/pause_restart_test.sh.
It ends with one summary line starting "PAUSE RESTART:" and exits nonzero on any violation.
"""

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(REPO_ROOT, 'clients', 'python'))
try:
    import orly  # noqa: E402
except ImportError as ex:
    sys.exit(f'pause restart test: cannot start: {ex} (install websocket-client for the python3 that runs this)')

PKG = 'pause_restart'

PACKAGE = '''package #1;
put = ((true) effecting {
  new <['p', n]> <- n;
}) where {
  n = given::(int);
};
fill = ((true) effecting {
  new <['f', n]> <- s;
}) where {
  n = given::(int);
  s = given::(str);
};
paused_keys = (*(keys (int) @ <['p', free::(int)]>)::(int) as [int]);
'''

# name: (writes into the POV after its pause, write until the Pov records are copied forward,
#        an older paused POV holding the head below the pause, unpause before the SIGKILL,
#        times the paused POV itself must be copied forward first)
SCENARIOS = {
    'copied-empty': (0, True, False, False, 0),
    'copied-writes': (5, True, False, False, 0),
    'copied-held': (0, True, True, False, 0),
    'copied-unpaused': (0, True, False, True, 0),
    'no-ring': (5, False, False, False, 0),
    'no-ring-empty': (0, False, False, False, 0),
    'repo-copied': (5, True, False, False, 2),
    'repo-promoted': (5, True, False, True, 2),
}
COPIED = re.compile(r'TWalRetention: copied (\d+) Pov records forward')
COPIED_REPOS = re.compile(r'TWalRetention: copied (\d+) of \d+ paused repos forward')


def log(msg):
    print(msg, flush=True)


class Server:
    """One orlyi on its own loopback volume. Only its own PID is ever signalled."""

    def __init__(self, args, work, name):
        self.args, self.work, self.name = args, work, name
        self.instance = f'pr961_{uuid.uuid4().hex[:16]}'
        self.proc = None
        self.loop = None
        self.image = os.path.join(work, f'{name}.img')
        self.runs = 0
        self.logpath = None
        self.log = None

    def attach(self):
        self.loop = subprocess.check_output(['losetup', '-fP', '--show', self.image], text=True).strip()

    def detach(self):
        if self.loop:
            subprocess.call(['losetup', '-d', self.loop])
            self.loop = None

    def create_volume(self):
        subprocess.check_call(['truncate', '-s', f'{self.args.volume_gb}G', self.image])
        self.attach()
        subprocess.check_call([os.path.join(self.args.orly_out, 'orly/indy/disk/util/orly_dm'),
                               '--create-volume', '--device-speed=fast', f'--instance-name={self.instance}',
                               '--num-devices=1', os.path.basename(self.loop)],
                              stdout=subprocess.DEVNULL)

    def start(self, create):
        self.runs += 1
        self.logpath = os.path.join(self.work, f'{self.name}-orlyi-{self.runs}.log')
        p = self.args.port
        cmd = self.args.orlyi_prefix.split() + [
            os.path.join(self.args.orly_out, 'orly/server/orlyi'),
            f'--create={"true" if create else "false"}', f'--instance_name={self.instance}',
            '--starting_state=SOLO', f'--port_number={p}', f'--slave_port_number={p + 1}',
            f'--ws_port_number={p + 2}', f'--reporting_port_number={p + 3}',
            f'--package_dir={self.work}/packages', '--max_parallel_frames=4000',
            '--page_cache_size=256', '--block_cache_size=64', '--do_fsync', '--no_realtime',
            '--log_info', f'--wal_mb={self.args.wal_mb}']
        extra = self.args.extra_args.split()
        names = {a.split('=', 1)[0] for a in extra}
        cmd = [a for a in cmd if a.split('=', 1)[0] not in names] + extra
        self.log = open(self.logpath, 'w')
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + self.args.startup_timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return f'orlyi exited ({self.proc.returncode}) during startup'
            try:
                socket.create_connection(('127.0.0.1', p + 2), timeout=1).close()
                return None
            except OSError:
                time.sleep(0.5)
        return f'orlyi never opened its WebSocket port in {self.args.startup_timeout}s'

    def kill(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait()
        self.proc = None
        self.log.close()

    def stop(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(120)
            except subprocess.TimeoutExpired:
                os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait()
        self.proc = None
        self.log.close()

    def log_text(self):
        try:
            with open(self.logpath, errors='replace') as f:
                return f.read()
        except OSError:
            return ''

    def tail(self, n=20):
        return ''.join(self.log_text().splitlines(keepends=True)[-n:])

    def copies(self):
        return sum(int(m.group(1)) for m in COPIED.finditer(self.log_text()))

    def repo_copies(self):
        return sum(int(m.group(1)) for m in COPIED_REPOS.finditer(self.log_text()))


def visible_keys(c):
    """The 'p' keys a new POV of the global POV sees."""
    rpov = c.new_pov(safe=False, shared=False)
    return sorted(int(v) for v in c.call(rpov, PKG, 'paused_keys'))


def fill_until_copied(c, srv, args, need, repo_laps=0):
    """Write through a separate safe POV until `need` Pov records have been copied forward (the
    oldest are copied first, and the fill POV is the newest, so these are the paused POVs'), and
    paused POVs copied forward `repo_laps` times, then keep going a little so the checkpoint head
    moves on. Returns None or an error."""
    fpov = c.new_pov(safe=True, shared=True)
    blob = 'x' * args.fill_bytes
    n = 0
    deadline = time.monotonic() + args.fill_timeout
    copied_at = None
    while time.monotonic() < deadline:
        batch = [{'n': n + i, 's': blob} for i in range(args.fill_batch)]
        try:
            c.call_batch(fpov, PKG, 'fill', batch)
            n += args.fill_batch
        except (orly.InsufficientMemory, orly.InsufficientStorage):
            time.sleep(0.2)
            continue
        if copied_at is None and srv.copies() >= need and srv.repo_copies() >= repo_laps:
            copied_at = n
            log(f'  Pov records copied forward after {n} fill writes'
                + (f', paused POVs {srv.repo_copies()} times' if repo_laps else ''))
        if copied_at is not None and n >= copied_at + args.fill_after:
            # Let the checkpointer (once a second) write a checkpoint with the new head.
            time.sleep(3)
            return None
    return (f'not copied forward within {args.fill_timeout}s ({n} fill writes; {srv.copies()} Pov records, '
            f'{srv.repo_copies()} paused POVs of {repo_laps})')


def wait_visible(c, writes, timeout=30):
    """The 'p' keys global sees (but the holder's), once it sees `writes` of them or after `timeout`."""
    deadline = time.monotonic() + timeout
    while True:
        seen = [k for k in visible_keys(c) if k != 1000]
        if len(seen) >= writes or time.monotonic() >= deadline:
            return seen
        time.sleep(0.25)


def finish(srv, kv):
    if re.search(r'FATAL ERROR|TERMINATE', srv.log_text()):
        kv.append('the log shows a FATAL ERROR or TERMINATE')
    srv.stop()
    return kv


def run_scenario(args, work, name, kv):
    srv = Server(args, work, name)
    url = f'ws://127.0.0.1:{args.port + 2}/'
    try:
        srv.create_volume()
        err = srv.start(create=True)
        if err:
            return [f'start: {err}\n{srv.tail()}']
        with orly.connect(url, timeout=10, recv_timeout=120) as c:
            c.new_session()
            c.install(PKG, 1)
        # A graceful restart, so the install is on disk before the SIGKILL below.
        srv.stop()
        err = srv.start(create=False)
        if err:
            return [f'restart after the install: {err}\n{srv.tail()}']
        writes, fill, held, unpause, laps = SCENARIOS[name]
        want = 'normal' if unpause else 'paused'
        with orly.connect(url, timeout=10, recv_timeout=120) as c:
            sid = c.new_session()
            if held:
                # Its unpromoted write pins the head below the other POV's pause for good.
                holder = c.new_pov(safe=True, shared=True)
                c.pause(holder)
                c.call(holder, PKG, 'put', {'n': 1000})
            pov = c.new_pov(safe=True, shared=True)
            c.pause(pov)
            for i in range(1, writes + 1):
                c.call(pov, PKG, 'put', {'n': i})
            if fill:
                err = fill_until_copied(c, srv, args, 2 if held else 1, laps)
                if err:
                    return [err]
            if unpause:
                c.unpause(pov)
                time.sleep(3)
                if writes:
                    seen = wait_visible(c, writes)
                    if seen != list(range(1, writes + 1)):
                        kv.append(f'before the restart, after unpausing, global sees {seen}, not 1..{writes}')
                    # Let a checkpoint pass the copies, so recovery replays the promotions alone.
                    time.sleep(3)
            status = c.review(pov)['status']
            if status != want:
                kv.append(f'before the restart the POV is {status!r}, not {want}')
        srv.kill()
        srv.detach()
        srv.attach()
        err = srv.start(create=False)
        if err:
            return kv + [f'restart: {err}\n{srv.tail()}']
        with orly.connect(url, timeout=10, recv_timeout=120) as c:
            c.resume_session(sid)
            review = c.review(pov)
            log(f'  after the restart: status {review["status"]}, {review["pending"]} pending')
            if review['status'] != want:
                kv.append(f'after the restart the POV is {review["status"]!r}, not {want}')
            pending = 0 if unpause else writes
            if int(review['pending']) != pending:
                kv.append(f'after the restart the POV has {review["pending"]} updates pending, not {pending}')
            # Give Tetris time to promote what it shouldn't.
            time.sleep(args.settle)
            seen = [k for k in visible_keys(c) if k != 1000]
            if unpause:
                # Promoted before the SIGKILL: exactly once, from whichever records recovery read.
                if seen != list(range(1, writes + 1)):
                    kv.append(f'after the restart global sees {seen}, not 1..{writes}')
            elif seen:
                kv.append(f'the paused POV\'s updates were promoted after the restart: global sees {seen}')
            if review['status'] != 'paused':
                return finish(srv, kv)
            c.unpause(pov)
            seen = wait_visible(c, writes)
            if seen != list(range(1, writes + 1)):
                kv.append(f'after unpausing, global sees {seen}, not 1..{writes}')
            if held:
                if c.review(holder)['status'] != 'paused':
                    kv.append('after the restart the older paused POV is not paused')
        return finish(srv, kv)
    finally:
        if srv.proc is not None:
            srv.kill()
        srv.detach()
    return kv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(REPO_ROOT, '../out_orly/debug')))
    ap.add_argument('--orlyi-prefix', default=os.environ.get('ORLYI_PREFIX', ''))
    ap.add_argument('--extra-args', default=os.environ.get('EXTRA_ARGS', ''),
                    help='more orlyi flags; one named here replaces the default of the same name')
    ap.add_argument('--scenarios', default=','.join(SCENARIOS), help=f'comma-separated, of {", ".join(SCENARIOS)}')
    ap.add_argument('--port', type=int, default=19960)
    ap.add_argument('--volume-gb', type=int, default=2)
    ap.add_argument('--wal-mb', type=int, default=8)
    ap.add_argument('--fill-bytes', type=int, default=2000, help='bytes per fill value')
    ap.add_argument('--fill-batch', type=int, default=8, help='fill writes per transaction')
    ap.add_argument('--fill-after', type=int, default=48, help='fill writes after the copy')
    ap.add_argument('--fill-timeout', type=float, default=300)
    ap.add_argument('--settle', type=float, default=5, help='seconds to wait before checking global')
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('pause_restart_test.py needs root (losetup); run tests/pause_restart_test.sh')
    work = tempfile.mkdtemp(prefix='pause961.')
    violations = []
    try:
        os.makedirs(f'{work}/packages')
        open(f'{work}/packages/__orly__', 'w').close()
        with open(f'{work}/{PKG}.orly', 'w') as f:
            f.write(PACKAGE)
        subprocess.check_call(args.orlyi_prefix.split() + [os.path.join(args.orly_out, 'orly/orlyc'),
                              '--skip-tests', '-o', work, f'{work}/{PKG}.orly'], cwd=work,
                              stdout=subprocess.DEVNULL)
        shutil.copy(f'{work}/{PKG}.1.so', f'{work}/packages/')
        for name in args.scenarios.split(','):
            began = time.monotonic()
            kv = []
            try:
                kv = run_scenario(args, work, name, kv)
            except Exception as ex:  # noqa: BLE001
                kv.append(f'{type(ex).__name__}: {ex}')
            log(f'{name}: {"ok" if not kv else "FAILED"} ({time.monotonic() - began:.0f}s)')
            for v in kv:
                log(f'  VIOLATION: {v}')
            violations += [f'{name}: {v}' for v in kv]
    finally:
        if not args.keep and not violations:
            shutil.rmtree(work, ignore_errors=True)
        else:
            log(f'work directory kept: {work}')
    log(f'PAUSE RESTART: scenarios={args.scenarios} violations={len(violations)}')
    sys.exit(1 if violations else 0)


if __name__ == '__main__':
    main()
