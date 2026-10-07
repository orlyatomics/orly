#!/usr/bin/env python3
"""Graceful stop under write load (#744): SIGTERM a disk-backed orlyi while eight writers are
writing to it, require it to stop on its own within a deadline, restart it on the same volume,
and require every acknowledged write back.

A graceful stop flushes (#440): Tetris promotes what the POVs hold, the memory merges write it
out, and the durable writer writes the session and POV records. So unlike a SIGKILL (#730's
campaign), a graceful stop may lose nothing that was acknowledged. Before #744's fix it mostly
didn't stop at all under this load: the durable layer cleaner was stopped before the flush, the
Durable Layer pool ran dry, the writer fiber died of the bad_alloc, and orlyi hung.

Per stop it checks:

  deadline   orlyi exits within --deadline seconds of the SIGTERM, with status 0, after logging
             "TServer::Shutdown() complete"
  clean      the log has no fiber that died of an exception ("FATAL ERROR"), no TERMINATE, and
             no Durable Layer pool miss
  acked      every write acknowledged before the stop comes back after the restart
  prefix     each writer's keys are exactly 1..R, no holes, and R is at most what it sent
  counter    each writer's `+=` counter equals R, and the shared `+=` total equals the sum of R

The writers use the same mix as the kill campaign: safe and fast POVs, shared and private,
nested, single writes and batches. It ends with one summary line and exits nonzero on any
violation. Needs root (losetup and the /proc/partitions scan, as tests/restart_test.sh); run it
through tests/graceful_stop_test.sh. It only ever signals the orlyi it started.
"""

import argparse
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(REPO_ROOT, 'clients', 'python'))
try:
    import orly  # noqa: E402
except ImportError as ex:
    sys.exit(f'graceful stop test: cannot start: {ex} (install websocket-client for the python3 that runs this)')

PKG = 'graceful_stop'

# One call is one write of writer w: its n-th key, its own `+=` counter and the shared `+=`
# total, in one transaction.
PACKAGE = '''package #1;
put = ((true) effecting {
  new <['w', w, n]> <- n;
  *<['c', w]>::(int) += 1;
  *<['t']>::(int) += 1;
}) where {
  w = given::(int);
  n = given::(int);
};
vals_of = (*(keys (int) @ <['w', w, free::(int)]>)::(int) as [int]) where { w = given::(int); };
count_of = (((*<['c', w]>::(int)) if *<['c', w]>::(int?) is known else 0)) where { w = given::(int); };
total = ((*<['t']>::(int)) if *<['t']>::(int?) is known else 0);
'''

# (name, safe, shared, batch size, POV group, parent group), as in tests/kill_campaign.py.
WRITERS = [
    ('safe-shared-single', True, True, 1, 'A', None),
    ('safe-private-batch4', True, False, 4, 'B', None),
    ('fast-shared-single', False, True, 1, 'C', None),
    ('fast-private-batch4', False, False, 4, 'D', None),
    ('safe-shared-pair1', True, True, 1, 'E', None),
    ('safe-shared-pair2-batch3', True, True, 3, 'E', None),
    ('safe-nested-single', True, True, 1, 'F', 'A'),
    ('fast-nested-batch2', False, True, 2, 'G', 'C'),
]

LOG_BAD = re.compile(r'FATAL ERROR|TERMINATE|\[Durable Layer\] bad_alloc')


def log(msg):
    print(msg, flush=True)


class Writer:
    """One client connection writing keys 1, 2, 3, ... in order until the server goes away."""

    def __init__(self, idx, spec):
        self.idx = idx
        self.name, self.safe, self.shared, self.batch, self.group, self.parent = spec
        self.sent = 0      # highest key sent (acknowledged or in flight)
        self.acked = 0     # highest key acknowledged
        self.calls = 0     # transactions acknowledged this round
        self.error = None  # an error before the stop
        self.thread = None

    def run(self, url, pov, stopping):
        try:
            c = orly.connect(url, timeout=10, recv_timeout=120, retries=3)
            c.new_session()
            if pov is None:
                pov = c.new_pov(safe=self.safe, shared=self.shared)
        except Exception as ex:  # noqa: BLE001
            if not stopping.is_set():
                self.error = f'setup: {ex}'
            return
        try:
            while True:
                start = self.sent + 1
                end = start + self.batch - 1
                self.sent = end
                args = [{'w': self.idx, 'n': n} for n in range(start, end + 1)]
                try:
                    if self.batch == 1:
                        c.call(pov, PKG, 'put', args[0])
                    else:
                        c.call_batch(pov, PKG, 'put', args)
                except (orly.InsufficientMemory, orly.InsufficientStorage):
                    # Refused, nothing written (docs/PROTOCOL.md): take it back and retry.
                    self.sent = start - 1
                    time.sleep(0.05)
                    continue
                except Exception as ex:  # noqa: BLE001
                    # The stop closes the connection, mid-call or not: this write stays in
                    # flight. Anything before the stop is a failure.
                    if not stopping.is_set():
                        time.sleep(0.5)
                        if not stopping.is_set():
                            self.error = f'write {start}..{end}: {ex}'
                    return
                self.acked = end
                self.calls += 1
        finally:
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass


class Server:
    """The one orlyi this driver runs. Only its own PID is ever signalled."""

    def __init__(self, args, work):
        self.args, self.work = args, work
        self.instance = f'gs744_{uuid.uuid4().hex[:16]}'
        self.proc = None
        self.loop = None
        self.image = os.path.join(work, 'disk.img')
        self.runs = 0
        self.logpath = None

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
        self.logpath = os.path.join(self.work, f'orlyi-{self.runs:03d}.log')
        p = self.args.port
        cmd = self.args.orlyi_prefix.split() + [
            os.path.join(self.args.orly_out, 'orly/server/orlyi'),
            f'--create={"true" if create else "false"}', f'--instance_name={self.instance}',
            '--starting_state=SOLO', f'--port_number={p}', f'--slave_port_number={p + 1}',
            f'--ws_port_number={p + 2}', f'--reporting_port_number={p + 3}',
            '--connection_backlog=32', f'--package_dir={self.work}/packages',
            '--max_parallel_frames=4000', '--page_cache_size=256', '--block_cache_size=64',
            '--do_fsync', '--no_realtime', '--log_info',
            f'--update_pool_size={self.args.update_pool_size}',
            f'--update_entry_pool_size={self.args.update_pool_size * 2}']
        # A flag in --extra-args replaces the default of the same name: orlyi refuses a flag
        # given twice.
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

    def stop(self, deadline):
        """SIGTERM our orlyi and wait up to 'deadline' seconds. Returns (seconds it took or None
        if it was still running, its exit status). A server still running is SIGKILLed."""
        if self.proc is None:
            return None, None
        signalled = time.monotonic()
        early = self.proc.poll()
        took = 0.0
        if early is None:
            os.kill(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(deadline)
                took = time.monotonic() - signalled
            except subprocess.TimeoutExpired:
                took = None
                os.kill(self.proc.pid, signal.SIGKILL)
        rc = self.proc.wait()
        self.proc = None
        self.log.close()
        return took, rc

    def tail(self, n=15):
        try:
            with open(self.logpath, errors='replace') as f:
                return ''.join(f.readlines()[-n:])
        except OSError:
            return ''

    def log_text(self):
        with open(self.logpath, errors='replace') as f:
            return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stops', type=int, default=int(os.environ.get('STOPS') or 4))
    ap.add_argument('--deadline', type=float, default=float(os.environ.get('DEADLINE') or 60),
                    help='seconds a stop may take (default 60)')
    ap.add_argument('--seed', type=int, default=int(os.environ.get('SEED') or 0) or int(time.time()))
    ap.add_argument('--min-run', type=float, default=1.5, help='shortest load before a stop (s)')
    ap.add_argument('--max-run', type=float, default=3.0, help='longest load before a stop (s)')
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(REPO_ROOT, '../out_orly/release')))
    ap.add_argument('--orlyi-prefix', default=os.environ.get('ORLYI_PREFIX', ''),
                    help='command to run orlyi under, e.g. "setarch x86_64 -R" for a TSan build')
    ap.add_argument('--extra-args', default=os.environ.get('EXTRA_ARGS', ''),
                    help='more orlyi flags; one named here replaces the default of the same name')
    ap.add_argument('--port', type=int, default=19940)
    ap.add_argument('--volume-gb', type=int, default=3)
    ap.add_argument('--update-pool-size', type=int, default=100000)
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('graceful_stop_test.py needs root (losetup); run tests/graceful_stop_test.sh')
    rng = random.Random(args.seed)
    work = tempfile.mkdtemp(prefix='graceful744.')
    srv = Server(args, work)
    url = f'ws://127.0.0.1:{args.port + 2}/'
    violations = []
    stop_times = []
    acked_total = 0
    try:
        log(f'graceful stop test: stops={args.stops} deadline={args.deadline:g}s seed={args.seed} '
            f'instance={srv.instance} work={work}')
        os.makedirs(f'{work}/packages')
        open(f'{work}/packages/__orly__', 'w').close()
        with open(f'{work}/{PKG}.orly', 'w') as f:
            f.write(PACKAGE)
        subprocess.check_call(args.orlyi_prefix.split() + [os.path.join(args.orly_out, 'orly/orlyc'),
                              '--skip-tests', '-o', work, f'{work}/{PKG}.orly'], cwd=work,
                              stdout=subprocess.DEVNULL)
        shutil.copy(f'{work}/{PKG}.1.so', f'{work}/packages/')
        srv.create_volume()
        err = srv.start(create=True)
        if err:
            sys.exit(f'{err}\n{srv.tail()}')
        with orly.connect(url, timeout=10, recv_timeout=120) as c:
            c.new_session()
            c.install(PKG, 1)
        writers = [Writer(i, spec) for i, spec in enumerate(WRITERS)]
        floor = {w.idx: 0 for w in writers}
        for k in range(1, args.stops + 1):
            setup = orly.connect(url, timeout=10, recv_timeout=120)
            setup.new_session()
            povs = {}
            for w in writers:
                if w.shared and w.group not in povs and w.parent is None:
                    povs[w.group] = setup.new_pov(safe=w.safe, shared=True)
            for w in writers:
                if w.parent is not None and w.group not in povs:
                    povs[w.group] = setup.new_pov(safe=w.safe, shared=True, parent=povs[w.parent])
            stopping = threading.Event()
            for w in writers:
                w.error = None
                w.calls = 0
                pov = povs.get(w.group) if w.shared else None
                w.thread = threading.Thread(target=w.run, args=(url, pov, stopping), daemon=True)
                w.thread.start()
            run_for = rng.uniform(args.min_run, args.max_run)
            time.sleep(run_for)
            # Stop while the writers are still writing, as `docker stop` would.
            stopping.set()
            took, rc = srv.stop(args.deadline)
            for w in writers:
                w.thread.join(60)
            try:
                setup.close()
            except Exception:  # noqa: BLE001
                pass
            kv = []
            calls = sum(w.calls for w in writers)
            if took is None:
                kv.append(f'orlyi was still running {args.deadline:g}s after SIGTERM (SIGKILLed)')
            else:
                stop_times.append(took)
            text = srv.log_text()
            if took is not None and 'TServer::Shutdown() complete' not in text:
                kv.append('orlyi exited without completing its shutdown')
            if took is not None and rc != 0:
                kv.append(f'orlyi exit status {rc}')
            bad = sorted({m.group(0) for m in LOG_BAD.finditer(text)})
            if bad:
                first = next(line for line in text.splitlines() if LOG_BAD.search(line))
                kv.append(f'log shows {", ".join(bad)}: {first.strip()[:200]}')
            for w in writers:
                if w.thread.is_alive():
                    kv.append(f'{w.name}: writer still blocked 60s after the stop')
                if w.error:
                    kv.append(f'{w.name}: {w.error}')
            srv.detach()
            srv.attach()
            err = srv.start(create=False)
            if err:
                kv.append(f'restart: {err}')
                log(f'stop {k}: RESTART FAILED: {err}\n{srv.tail()}')
                violations += [f'stop {k}: {v}' for v in kv]
                break
            lost = 0
            with orly.connect(url, timeout=10, recv_timeout=300) as c:
                c.new_session()
                rpov = c.new_pov(safe=False, shared=False)
                total = int(c.call(rpov, PKG, 'total'))
                sum_r = 0
                for w in writers:
                    ns = [int(v) for v in c.call(rpov, PKG, 'vals_of', {'w': w.idx})]
                    count = int(c.call(rpov, PKG, 'count_of', {'w': w.idx}))
                    r = len(ns)
                    sum_r += r
                    if ns != list(range(1, r + 1)):
                        holes = sorted(set(range(1, max(ns, default=0) + 1)) - set(ns))[:5]
                        kv.append(f'{w.name}: keys are not a prefix (holes at {holes}, {r} keys)')
                    if r < w.acked:
                        lost += w.acked - r
                        kv.append(f'{w.name}: {w.acked - r} acknowledged writes lost (acked 1..{w.acked}, got 1..{r})')
                    if r < floor[w.idx]:
                        kv.append(f'{w.name}: writes from an earlier restart are gone (had {floor[w.idx]}, now {r})')
                    if r > w.sent:
                        kv.append(f'{w.name}: {r - w.sent} writes came back that were never sent')
                    if count != r:
                        kv.append(f'{w.name}: `+=` counter is {count} with {r} keys')
                    floor[w.idx] = w.sent = w.acked = r
                if total != sum_r:
                    kv.append(f'shared `+=` total is {total} with {sum_r} keys in all')
            acked_total += calls
            took_s = f'{took:.1f}s' if took is not None else f'>{args.deadline:g}s (hung)'
            log(f'stop {k}: after {run_for:.1f}s of load, {calls} txns acknowledged; stop took {took_s}; '
                f'keys {sum_r}; acknowledged writes lost {lost}' + ('' if not kv else f'; VIOLATIONS: {len(kv)}'))
            for v in kv:
                log(f'  VIOLATION: {v}')
            violations += [f'stop {k}: {v}' for v in kv]
        srv.stop(args.deadline)
    finally:
        if srv.proc is not None:
            srv.stop(args.deadline)
        srv.detach()
        if not args.keep and not violations:
            shutil.rmtree(work, ignore_errors=True)
        elif violations:
            log(f'work directory kept for the evidence: {work}')
    done = len(stop_times)
    log(f'GRACEFUL STOP: stops={args.stops} finished={done} '
        f'max_stop_s={max(stop_times, default=0):.1f} acked_txns={acked_total} '
        f'violations={len(violations)} seed={args.seed}')
    sys.exit(1 if violations or done < args.stops else 0)


if __name__ == '__main__':
    main()
