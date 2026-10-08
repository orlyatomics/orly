#!/usr/bin/env python3
"""Kill-and-recover campaign (#730): SIGKILL a disk-backed orlyi under write load, restart it on
the same volume, and check what came back against a client-side ledger.

The contract it checks is the one in docs/durability.md, read from the code:

  * An acknowledgment means the write committed to its POV in memory, on safe and fast POVs
    alike. Nothing is on disk at that point. A write reaches disk only after Tetris has promoted
    it to the global POV and the global POV's next memory merge has written it to a data file.
  * So a SIGKILL may lose acknowledged writes, but only the newest ones: for each POV, what
    comes back is a prefix of the order its writes committed in.

Per kill, for every writer it checks:

  prefix       the writer's keys that came back are exactly 1..R, no holes
  floor        R is at least what the previous restart gave back (a write once read back after
               a restart must never disappear)
  phantom      R is at most the last write the writer sent (acknowledged or in flight), and each
               value carries the epoch that last wrote that key
  atomic       R ends on a transaction boundary: a batch comes back whole or not at all
  counter      the writer's `+=` counter equals R (not double-applied, not lost apart from its
               key), and the shared `+=` total equals the sum of every R
  bound        the acknowledged transactions lost, over all writers, are at most the updates
               the Update pool held just before the kill (read from the reporting port) plus the
               transactions acknowledged after that look: every acknowledged transaction that
               isn't on disk yet holds at least one update in that pool, so the pool's size,
               --update_pool_size, bounds any kill's loss
  progress     (over the whole campaign) some of every writer's writes came back after some
               restart: a writer whose POV Tetris failed never gets any back (#751)
  open         the restart succeeds, and the #700 open check finds nothing worse than leaked
               blocks or a merge's leftover (nested) input
  ephemeral    a POV from before the kill is refused, not resurrected (#439)
  live         the restarted server keeps acknowledging writes: a load of at least
               --wedge-after seconds that acknowledges fewer than --wedge-min-txns transactions
               is a wedge (#804). Before the kill, the driver saves every thread's state and
               stacks of that orlyi (wedge-<k>/ in the work directory), so a SIGKILL run catches
               it too, not only a SIGTERM stop that then hangs

It ends with one summary line and exits nonzero on any violation.

--negative=rollback is the negative control: before the restart after kill 3 (and every second
kill after that) it puts back the volume image saved at the kill before last, which is how a lost
or unsynced data file would look. The floor check must catch it, and CI requires that run to fail.

Needs root (losetup and the /proc/partitions scan, as tests/restart_test.sh); run it through
tests/kill_campaign.sh. It only ever signals the orlyi it started.
"""

import argparse
import collections
import os
import random
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import glob
import threading
import time
import uuid

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(REPO_ROOT, 'clients', 'python'))
try:
    import orly  # noqa: E402
except ImportError as ex:
    sys.exit(f'kill campaign: cannot start: {ex} (install websocket-client for the python3 that runs this)')

PKG = 'kill_campaign'

# (name, safe, shared, batch size, POV group, parent group, how it writes). Writers in one group
# share its POV, which must then be shared. A parent group makes the POV a child of that group's
# POV, so its writes are promoted twice before they reach global. 'put' writes with the `put`
# method; 'cond' with `put_cond`, whose value is an `if`, so Tetris replays each call before it
# promotes the batch (#751); 'mixed' alternates the two in one call_many batch (#255).
WRITERS = [
    ('safe-shared-single', True, True, 1, 'A', None, 'put'),
    ('safe-private-batch4', True, False, 4, 'B', None, 'put'),
    ('fast-shared-single', False, True, 1, 'C', None, 'put'),
    ('fast-private-batch4', False, False, 4, 'D', None, 'put'),
    ('safe-shared-pair1', True, True, 1, 'E', None, 'put'),
    ('safe-shared-pair2-batch3', True, True, 3, 'E', None, 'put'),
    ('safe-nested-single', True, True, 1, 'F', 'A', 'put'),
    ('fast-nested-batch2', False, True, 2, 'G', 'C', 'put'),
    ('safe-shared-cond-batch3', True, True, 3, 'H', None, 'cond'),
    ('fast-private-mixed-batch2', False, False, 2, 'I', None, 'mixed'),
]


def log(msg):
    print(msg, flush=True)


class Txn:
    __slots__ = ('start', 'end', 'acked_at')

    def __init__(self, start, end):
        self.start, self.end, self.acked_at = start, end, None


class Writer:
    """One client connection writing keys 1, 2, 3, ... in order, and its ledger."""

    def __init__(self, idx, spec):
        self.idx = idx
        self.name, self.safe, self.shared, self.batch, self.group, self.parent, self.how = spec
        self.floor = 0            # keys 1..floor came back after a restart
        self.epoch_of = [0]       # epoch_of[n]: the epoch that last sent key n
        self.txns = []            # transactions sent since the last restart
        self.sent = 0             # highest key sent (acknowledged or in flight)
        self.acked = 0            # highest key acknowledged
        self.error = None         # an error while the server was meant to be up
        self.refused = 0          # writes refused as insufficient_memory/storage this epoch
        self.first_ack = None     # when this epoch's first write was acknowledged
        self.thread = None

    def run(self, url, pov, epoch, killed):
        try:
            c = orly.connect(url, timeout=10, recv_timeout=120, retries=3)
            c.new_session()
            if pov is None:
                pov = c.new_pov(safe=self.safe, shared=self.shared)
        except Exception as ex:  # noqa: BLE001
            if not killed.is_set():
                self.error = f'setup: {ex}'
            return
        try:
            while not killed.is_set():
                start = self.sent + 1
                end = start + self.batch - 1
                while len(self.epoch_of) <= end:
                    self.epoch_of.append(0)
                prior = self.epoch_of[start:end + 1]
                for n in range(start, end + 1):
                    self.epoch_of[n] = epoch
                txn = Txn(start, end)
                self.txns.append(txn)
                self.sent = end
                args = [{'w': self.idx, 'n': n, 'e': epoch} for n in range(start, end + 1)]
                try:
                    if self.how == 'mixed':
                        c.call_many(pov, [(PKG, 'put_cond' if i % 2 else 'put', a) for i, a in enumerate(args)])
                    elif self.batch == 1:
                        c.call(pov, PKG, 'put_cond' if self.how == 'cond' else 'put', args[0])
                    else:
                        c.call_batch(pov, PKG, 'put_cond' if self.how == 'cond' else 'put', args)
                except (orly.InsufficientMemory, orly.InsufficientStorage) as ex:
                    if 'this POV is failed' in str(ex):
                        # Not back-pressure: Tetris failed this POV, so its acknowledged writes
                        # will never be promoted (#751). Retrying can't help.
                        self.error = f'write {start}..{end}: {ex}'
                        return
                    # Refused, nothing written (docs/PROTOCOL.md): take it back and retry.
                    self.txns.pop()
                    self.sent = start - 1
                    self.epoch_of[start:end + 1] = prior
                    self.refused += 1
                    time.sleep(0.05)
                    continue
                except Exception as ex:  # noqa: BLE001
                    # The kill closes the socket mid-call: this write stays in flight, and may or
                    # may not have committed. Anything else, before the kill, is a failure.
                    if not killed.is_set():
                        time.sleep(0.5)  # a refused socket may just be the kill landing now
                        if not killed.is_set():
                            self.error = f'write {start}..{end}: {ex}'
                    return
                txn.acked_at = time.monotonic()
                if self.first_ack is None:
                    self.first_ack = txn.acked_at
                self.acked = end
        finally:
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass


class Server:
    """The one orlyi this driver runs. Only its own PID is ever signalled."""

    def __init__(self, args, work):
        self.args, self.work = args, work
        self.instance = f'kc730_{uuid.uuid4().hex[:16]}'
        self.proc = None
        self.loop = None
        self.image = os.path.join(work, 'disk.img')
        self.runs = 0

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
        cmd = [os.path.join(self.args.orly_out, 'orly/server/orlyi'),
               f'--create={"true" if create else "false"}', f'--instance_name={self.instance}',
               '--starting_state=SOLO', f'--port_number={p}', f'--slave_port_number={p + 1}',
               f'--ws_port_number={p + 2}', f'--reporting_port_number={p + 3}',
               '--connection_backlog=32', f'--package_dir={self.work}/packages',
               '--max_parallel_frames=4000', '--page_cache_size=256', '--block_cache_size=64',
               '--do_fsync', '--no_realtime', '--log_info',
               f'--update_pool_size={self.args.update_pool_size}',
               f'--update_entry_pool_size={self.args.update_pool_size * 2}']
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

    def kill(self, sig=signal.SIGKILL):
        """Signal our orlyi and reap it. Returns (its exit status if it had already exited, whether
        a SIGTERM failed to stop it within 360 s, so that it took a SIGKILL). 360 s is orlyi's
        default --stop_promote_budget_s plus the flush (#769)."""
        if self.proc is None:
            return None, False
        early = self.proc.poll()
        hung = False
        if early is None:
            os.kill(self.proc.pid, sig)
            try:
                self.proc.wait(None if sig == signal.SIGKILL else 360)
            except subprocess.TimeoutExpired:
                hung = True
                log(f'  stop hung: {capture_threads(self.proc.pid, os.path.join(self.work, f"stop-hang-{self.runs:03d}"))}')
                os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait()
        self.proc = None
        self.log.close()
        return early, hung

    def tail(self, n=15):
        try:
            with open(self.logpath, errors='replace') as f:
                return ''.join(f.readlines()[-n:])
        except OSError:
            return ''


def sample_report(port):
    """The Update pool's used count and the global repo's memory layers, from the reporting port,
    or None if it doesn't answer within 5 s. The reporter keeps the connection open, so read up
    to the Memory Admission line, which comes after the Global Layers one."""
    body = ''
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
            sock.sendall(b'GET / HTTP/1.0\r\n\r\n')
            while not re.search(r'^Memory Admission = .*\n', body, re.M):
                b = sock.recv(65536)
                if not b:
                    break
                body += b.decode(errors='replace')
    except OSError:
        return None
    pool = re.search(r'Update pool (\d+) / (\d+)', body)
    layers = re.search(r'Global Layers = disk (\d+); memory (\d+)', body)
    if not pool:
        return None
    return {'pool_used': int(pool.group(1)), 'pool_max': int(pool.group(2)),
            'global_mem_layers': int(layers.group(2)) if layers else -1}


OPEN_BAD = re.compile(r'open check: .*(FREE or waiting|owned twice|outside every volume|partial overlap|inverted)'
                      r'|open check failed')


def open_check(logpath):
    """(violation or None, leaked blocks, nested leftovers) from the #700 open check's lines."""
    leaked = nested = 0
    seen = False
    with open(logpath, errors='replace') as f:
        for line in f:
            if 'open check' not in line:
                continue
            seen = True
            if OPEN_BAD.search(line):
                return line.strip()[:300], leaked, nested
            m = re.search(r'open check: (\d+) LEAKED blocks', line)
            if m:
                leaked += int(m.group(1))
            if ': nested' in line or 'leftover input' in line:
                nested += 1
    if not seen:
        return 'no open check line in the log (did it run?)', leaked, nested
    return None, leaked, nested


def capture_threads(pid, out_dir):
    """Save the state of every thread of our orlyi (pid) to out_dir: per thread its name, kernel
    state, wait channel and syscall line (/proc/<pid>/task/*), then eu-stack's (or gdb's) stacks.
    Checks /proc/<pid>/comm first, since orlyi rewrites its argv (so cmdline can't identify it).
    Returns a one-line summary for the log."""
    os.makedirs(out_dir, exist_ok=True)

    def read(path):
        try:
            with open(path, errors='replace') as f:
                return f.read().strip()
        except OSError as ex:
            return f'<{ex.strerror}>'

    comm = read(f'/proc/{pid}/comm')
    if comm != 'orlyi':
        return f'pid {pid} is {comm!r}, not orlyi: nothing captured'
    states = collections.Counter()
    tasks = []
    for task in sorted(glob.glob(f'/proc/{pid}/task/*'), key=lambda t: int(os.path.basename(t))):
        stat = read(f'{task}/stat')
        # The state is the field after the parenthesized name, which may itself hold spaces.
        state = stat[stat.rfind(')') + 2:].split(' ', 1)[0] if ')' in stat else '?'
        wchan = read(f'{task}/wchan')
        states[(state, wchan)] += 1
        tasks.append([os.path.basename(task), read(f'{task}/comm'), state, wchan, read(f'{task}/syscall')])
    # A thread blocked locking a pthread mutex waits on the mutex's first word, and glibc keeps the
    # owner's tid two words in (__owner, on x86-64 and aarch64 alike). Name that owner when it is
    # one of our threads: a waiter whose owner is itself blocked on the same lock, or is a fiber
    # runner that went on to other work, is a lock held across a fiber switch.
    tids = {t[0] for t in tasks}
    futex_nr = {'x86_64': '202', 'aarch64': '98'}.get(os.uname().machine)
    owners = {}
    try:
        with open(f'/proc/{pid}/mem', 'rb', buffering=0) as mem:
            for t in tasks:
                call = t[4].split()
                if len(call) > 1 and call[0] == futex_nr:
                    addr = int(call[1], 16)
                    if addr not in owners:
                        try:
                            mem.seek(addr + 8)
                            owner = str(int.from_bytes(mem.read(4), 'little'))
                        except (OSError, ValueError, OverflowError):
                            owner = ''
                        owners[addr] = owner if owner in tids else ''
                    t.append(owners[addr])
    except OSError:
        pass
    with open(os.path.join(out_dir, 'tasks.txt'), 'w') as f:
        f.write('tid\tcomm\tstate\twchan\tsyscall\tmutex owner (if the futex is a pthread mutex)\n')
        f.write('\n'.join('\t'.join(t + [''] * (6 - len(t))) for t in tasks) + '\n')
    rows = tasks
    # The unwinder stops the process for a moment, which can interrupt an epoll_wait and make
    # orlyi abort on EINTR, so it goes last, after everything else is saved.
    stacks = 'no eu-stack or gdb'
    if shutil.which('eu-stack'):
        cmd = ['eu-stack', '-i', '-p', str(pid)]
    elif shutil.which('gdb'):
        cmd = ['gdb', '-p', str(pid), '-batch', '-ex', 'thread apply all bt']
    else:
        cmd = None
    if cmd:
        try:
            with open(os.path.join(out_dir, 'stacks.txt'), 'w') as f:
                subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=120)
            stacks = f'stacks by {cmd[0]}'
        except (OSError, subprocess.TimeoutExpired) as ex:
            stacks = f'{cmd[0]} failed: {ex}'
    top = ', '.join(f'{n} {st}/{wc}' for (st, wc), n in states.most_common(6))
    waiters = collections.Counter(t[5] for t in tasks if len(t) > 5 and t[5])
    held = ''.join(f'; {n} thread(s) wait on a mutex thread {o} holds' for o, n in waiters.most_common(3))
    return f'{len(rows)} threads ({top}){held}; {stacks}; saved in {out_dir}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kills', type=int, default=int(os.environ.get('KILLS') or 40))
    ap.add_argument('--seed', type=int, default=int(os.environ.get('SEED') or 0) or int(time.time()))
    ap.add_argument('--min-run', type=float, default=1.0, help='shortest load before a kill (s)')
    ap.add_argument('--max-run', type=float, default=6.0, help='longest load before a kill (s)')
    ap.add_argument('--signal', choices=['KILL', 'TERM'], default=os.environ.get('SIGNAL', 'KILL'),
                    help='KILL (the campaign) or TERM (a graceful stop, for comparison: same checks)')
    ap.add_argument('--negative', choices=['none', 'rollback'], default=os.environ.get('NEGATIVE', 'none'))
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(REPO_ROOT, '../out_orly/release')))
    ap.add_argument('--port', type=int, default=19900)
    ap.add_argument('--volume-gb', type=int, default=3)
    ap.add_argument('--update-pool-size', type=int, default=100000)
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--wedge-min-txns', type=int, default=100,
                    help='a load that acknowledges fewer transactions than this is a wedge (#804)')
    ap.add_argument('--wedge-after', type=float, default=1.0,
                    help='only loads at least this long (s) are checked for a wedge')
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('kill_campaign.py needs root (losetup); run tests/kill_campaign.sh')
    rng = random.Random(args.seed)
    work = tempfile.mkdtemp(prefix='kill730.')
    srv = Server(args, work)
    url = f'ws://127.0.0.1:{args.port + 2}/'
    violations = []
    per_kill = []
    try:
        log(f'kill campaign: kills={args.kills} signal={args.signal} seed={args.seed} '
            f'negative={args.negative} instance={srv.instance} work={work}')
        os.makedirs(f'{work}/packages')
        open(f'{work}/packages/__orly__', 'w').close()
        subprocess.check_call([os.path.join(args.orly_out, 'orly/orlyc'), '--skip-tests', '-o', work,
                               os.path.join(REPO_ROOT, 'tests/kill_campaign.orly')], cwd=work,
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
        snapshots = {}
        old_sessions = []
        for k in range(1, args.kills + 1):
            epoch = k
            # Fresh POVs each epoch: POVs don't survive a restart (#439).
            setup = orly.connect(url, timeout=10, recv_timeout=120)
            setup.new_session()
            old_sessions.append(setup.session_id)
            povs = {}
            for w in writers:
                if w.shared and w.group not in povs and w.parent is None:
                    povs[w.group] = setup.new_pov(safe=w.safe, shared=True)
            for w in writers:
                if w.parent is not None and w.group not in povs:
                    povs[w.group] = setup.new_pov(safe=w.safe, shared=True, parent=povs[w.parent])
            killed = threading.Event()
            started_at = time.monotonic()
            for w in writers:
                w.error = None
                w.refused = 0
                w.first_ack = None
                pov = povs.get(w.group) if w.shared else None
                w.thread = threading.Thread(target=w.run, args=(url, pov, epoch, killed), daemon=True)
                w.thread.start()
            run_for = rng.uniform(args.min_run, args.max_run)
            time.sleep(run_for)
            # The loss bound: every acknowledged transaction not yet on disk holds at least one
            # update in the Update pool, so what a kill can lose is at most what the pool holds
            # just before it, plus whatever is acknowledged after that look.
            sampled_at = time.monotonic()
            sample = sample_report(args.port + 3)
            # A restarted server that stops acknowledging writes (#804): save its threads before
            # the kill, which would end the evidence.
            acked_by_now = sum(1 for w in writers for t in list(w.txns) if t.acked_at is not None)
            wedged = None
            if run_for >= args.wedge_after and acked_by_now < args.wedge_min_txns and srv.proc.poll() is None:
                refused_by_now = sum(w.refused for w in writers)
                wedged = (f'wedged: only {acked_by_now} txns acknowledged in {run_for:.1f}s of load '
                          f'({refused_by_now} refused), under the {args.wedge_min_txns} a live server '
                          f'acknowledges (#804)')
                log(f'kill {k}: WEDGE: {acked_by_now} txns acknowledged in {run_for:.1f}s; saving threads')
                log(f'  {capture_threads(srv.proc.pid, os.path.join(work, f"wedge-{k:03d}"))}')
            killed.set()
            kill_at = time.monotonic()
            early, hung = srv.kill(signal.SIGTERM if args.signal == 'TERM' else signal.SIGKILL)
            for w in writers:
                w.thread.join(60)
            try:
                setup.close()
            except Exception:  # noqa: BLE001
                pass
            kv = []  # this kill's violations
            acked_txns = sum(1 for w in writers for t in w.txns if t.acked_at is not None)
            refused = sum(w.refused for w in writers)
            firsts = [w.first_ack - started_at for w in writers if w.first_ack is not None]
            first_ack = f'{min(firsts) * 1000:.0f} ms' if firsts else 'none'
            if early is not None:
                kv.append(f'orlyi exited on its own ({early}) before the kill')
            if wedged:
                kv.append(wedged)
            if hung:
                kv.append('orlyi was still running 360 s after SIGTERM')
            for w in writers:
                if w.thread.is_alive():
                    kv.append(f'{w.name}: writer still blocked 60s after the kill')
                if w.error:
                    kv.append(f'{w.name}: {w.error}')
            srv.detach()
            if args.negative == 'rollback':
                snapshots[k] = f'{work}/snap-{k}.img'
                subprocess.check_call(['cp', '--sparse=always', srv.image, snapshots[k]])
                if k >= 3 and k % 2 == 1:
                    log(f'  NEGATIVE CONTROL: putting back the volume image from kill {k - 2}')
                    subprocess.check_call(['cp', '--sparse=always', snapshots[k - 2], srv.image])
            srv.attach()
            err = srv.start(create=False)
            if err:
                kv.append(f'restart: {err}')
                log(f'kill {k}: RESTART FAILED: {err}\n{srv.tail()}')
                violations += [f'kill {k}: {v}' for v in kv]
                per_kill.append(None)
                break
            bad, leaked, nested = open_check(srv.logpath)
            if bad:
                kv.append(f'open check: {bad}')
            # Read back through a fresh POV, which reads through to global.
            lost_txns = lost_writes = in_flight_kept = 0
            max_age = 0.0
            with orly.connect(url, timeout=10, recv_timeout=300) as c:
                c.new_session()
                rpov = c.new_pov(safe=False, shared=False)
                total = int(c.call(rpov, PKG, 'total'))
                old_pov = povs['A']
                try:
                    c.call(old_pov, PKG, 'count_of', {'w': 0})
                    kv.append(f'pre-restart POV {old_pov} was resurrected (#439)')
                except orly.OrlyError as ex:
                    # 'ephemeral' when its record reloaded (#439); 'doesn't exist' when the record
                    # itself was newer than what reached disk. Either way it is refused.
                    if 'ephemeral' not in str(ex) and "doesn't exist" not in str(ex):
                        kv.append(f'pre-restart POV refused with the wrong error: {ex}')
                resumed = 0
                for sid in old_sessions[-3:]:
                    try:
                        with orly.connect(url, timeout=10, recv_timeout=60) as rc:
                            rc.resume_session(sid)
                            resumed += 1
                    except orly.OrlyError:
                        pass
                sum_r = 0
                w_txns = {w.idx: w.txns for w in writers}
                for w in writers:
                    vals = [int(v) for v in c.call(rpov, PKG, 'vals_of', {'w': w.idx})]
                    count = int(c.call(rpov, PKG, 'count_of', {'w': w.idx}))
                    ns = [v // 1000 for v in vals]
                    r = len(ns)
                    sum_r += r
                    if ns != list(range(1, r + 1)):
                        holes = sorted(set(range(1, max(ns, default=0) + 1)) - set(ns))[:5]
                        kv.append(f'{w.name}: keys are not a prefix (holes at {holes}, {r} keys, max {max(ns, default=0)})')
                    if r < w.floor:
                        kv.append(f'{w.name}: {w.floor - r} writes that a previous restart gave back are gone '
                                  f'(had 1..{w.floor}, now 1..{r})')
                    if r > w.sent:
                        kv.append(f'{w.name}: {r - w.sent} writes came back that were never sent (sent up to {w.sent})')
                    stale = [n for n, v in zip(ns, vals) if n < len(w.epoch_of) and v % 1000 != w.epoch_of[n]]
                    if stale:
                        n = stale[0]
                        kv.append(f'{w.name}: {len(stale)} keys hold an older epoch\'s value (key {n}: epoch '
                                  f'{vals[n - 1] % 1000}, last sent in {w.epoch_of[n]})')
                    ends = {w.floor} | {t.end for t in w.txns}
                    if r >= w.floor and r <= w.sent and r not in ends:
                        kv.append(f'{w.name}: {r} keys came back, which splits a transaction')
                    if count != r:
                        kv.append(f'{w.name}: `+=` counter is {count} with {r} keys')
                    for t in w.txns:
                        if t.acked_at is not None and t.end > r:
                            lost_txns += 1
                            lost_writes += t.end - max(t.start - 1, r)
                            max_age = max(max_age, kill_at - t.acked_at)
                        elif t.acked_at is None and t.start <= r:
                            in_flight_kept += 1
                    # The store is the truth from here on.
                    w.floor = w.sent = w.acked = r
                    w.txns = []
                if total != sum_r:
                    kv.append(f'shared `+=` total is {total} with {sum_r} keys in all')
            acked_after = sum(1 for w in writers for t in w_txns[w.idx]
                              if t.acked_at is not None and t.acked_at >= sampled_at)
            bound = (sample['pool_used'] if sample else args.update_pool_size) + acked_after
            if lost_txns > bound:
                kv.append(f'{lost_txns} acknowledged transactions lost, over the bound of {bound} '
                          f'(Update pool in use just before the kill, plus {acked_after} acknowledged after that)')
            per_kill.append((lost_writes, lost_txns, max_age, bound))
            log(f'kill {k}: after {run_for:.1f}s; {acked_txns} txns acknowledged (first after {first_ack}), '
                f'{refused} refused; keys {sum_r}; lost {lost_writes} writes in {lost_txns} '
                f'acknowledged txns (bound {bound}; oldest acked {max_age * 1000:.0f} ms before the kill); '
                f'global memory layers {sample["global_mem_layers"] if sample else "?"}; in-flight kept '
                f'{in_flight_kept}; sessions resumed {resumed}/{min(3, len(old_sessions))}; '
                f'open check leaked={leaked} nested={nested}' + ('' if not kv else f'; VIOLATIONS: {len(kv)}'))
            for v in kv:
                log(f'  VIOLATION: {v}')
            violations += [f'kill {k}: {v}' for v in kv]
        if len(per_kill) == args.kills and all(p is not None for p in per_kill):
            # A writer none of whose writes ever came back, after every restart, was never
            # promoted: its POV failed (#751) or its writes never left it.
            for w in writers:
                if w.floor == 0:
                    v = f'progress: none of {w.name}\'s writes came back after any of {args.kills} restarts'
                    log(f'  VIOLATION: {v}')
                    violations.append(v)
        srv.kill()
    finally:
        if srv.proc is not None:
            srv.kill()
        srv.detach()
        if not args.keep and not violations:
            shutil.rmtree(work, ignore_errors=True)
        elif violations:
            log(f'work directory kept for the evidence: {work}')
    done = [p for p in per_kill if p is not None]
    lost = [p[0] for p in done]
    summary = (f'KILL CAMPAIGN: kills={len(per_kill)} recovered={len(done)} '
               f'lost_per_kill=min {min(lost, default=0)} / median {statistics.median(lost) if lost else 0:g} / '
               f'max {max(lost, default=0)} max_lost={max(lost, default=0)} '
               f'max_lost_txns={max((p[1] for p in done), default=0)} '
               f'max_lost_age_ms={max((p[2] for p in done), default=0) * 1000:.0f} '
               f'min_bound_margin={min((p[3] - p[1] for p in done), default=0)} violations={len(violations)} signal={args.signal} seed={args.seed} negative={args.negative}')
    log(summary)
    sys.exit(1 if violations or len(done) < args.kills else 0)


if __name__ == '__main__':
    main()
