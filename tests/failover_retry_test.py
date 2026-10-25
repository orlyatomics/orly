#!/usr/bin/env python3
"""Exactly-once writes across a failover (#862): a slave keeps the operation ids its master's
writes carry, so after it is promoted a retry of one of those writes gets the saved outcome
instead of running again.

  1. A disk-backed master (--durable_acks=local) writes key 1 into a safe shared POV with
     operation id A, before any slave has joined.
  2. A slave joins. The master writes key 2 with operation id B, which replicates to the slave.
     Both servers are disk-backed with the log: the master's records carry the ids, and the
     slave logs what it applies and holds its checkpoint head until each id is in its table (#866).
  3. The master is SIGKILLed and the slave promotes.
  4. On the promoted slave:
       read     keys 1 and 2 are there, and the writer's `+=` counter is 2
       retry    B sent again replies with the saved outcome and doesn't run again: the counter
                stays 2
       join     A sent again is refused as operation_expired: it was written before the slave
                joined, so the slave can't know whether it applied
       fresh    once the join floor (the join, plus a minute for clock skew) has passed, a write
                with a new id applies

Each write is the kill campaign's `put_once`, which bumps the writer's `+=` counter, so a write
that runs twice shows in the counter.

--negative=noretry is the negative control: the slave runs with --operation_retention_count=0,
which turns its exactly-once table off. As before #862, the retry runs again and the counter goes
to 3, so the run must fail.

Needs root (losetup and the /proc/partitions scan, as tests/kill_campaign.py); run it through
tests/failover_retry_test.sh. It only ever signals the orlyi processes it started.
"""

import argparse
import os
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
    sys.exit(f'failover retry test: cannot start: {ex} (install websocket-client for the python3 that runs this)')

PKG = 'kill_campaign'
# The slave refuses unknown ids up to a minute past its join (operation_table.h, RefuseUnknownBefore).
JOIN_SKEW_S = 60


def log(msg):
    print(msg, flush=True)


class Server:
    """One orlyi on its own loopback volume. Only its own PID is ever signalled."""

    def __init__(self, args, work, name, port, mem_sim=False):
        self.args, self.work, self.name, self.port, self.mem_sim = args, work, name, port, mem_sim
        self.instance = f'fo862{name[0]}_{uuid.uuid4().hex[:16]}'
        self.image = os.path.join(work, f'{name}.img')
        self.logpath = os.path.join(work, f'{name}.log')
        self.loop = None
        self.proc = None
        self.log = None

    def create_volume(self):
        if self.mem_sim:
            return
        subprocess.check_call(['truncate', '-s', f'{self.args.volume_gb}G', self.image])
        self.loop = subprocess.check_output(['losetup', '-fP', '--show', self.image], text=True).strip()
        subprocess.check_call([os.path.join(self.args.orly_out, 'orly/indy/disk/util/orly_dm'),
                               '--create-volume', '--device-speed=fast', f'--instance-name={self.instance}',
                               '--num-devices=1', os.path.basename(self.loop)],
                              stdout=subprocess.DEVNULL)

    def start(self, state, master_slave_port=None):
        p = self.port
        cmd = [os.path.join(self.args.orly_out, 'orly/server/orlyi'),
               '--create=true', f'--instance_name={self.instance}', f'--starting_state={state}',
               f'--port_number={p}', f'--replica_port_number={p + 1}', f'--ws_port_number={p + 2}',
               f'--reporting_port_number={p + 3}', '--connection_backlog=32',
               f'--package_dir={self.work}/packages', '--max_parallel_frames=4000',
               '--page_cache_size=256', '--block_cache_size=64', '--log_info', '--log_notice']
        if self.mem_sim:
            cmd += ['--mem_sim', '--mem_sim_mb=512', '--mem_sim_slow_mb=256']
        else:
            cmd += ['--do_fsync', '--no_realtime', '--durable_acks=local']
        if master_slave_port is not None:
            cmd.append(f'--address_of_primary=127.0.0.1:{master_slave_port}')
            if self.args.negative == 'noretry':
                cmd.append('--operation_retention_count=0')
        self.log = open(self.logpath, 'w')
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + self.args.startup_timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return f'{self.name} exited ({self.proc.returncode}) during startup'
            try:
                socket.create_connection(('127.0.0.1', p + 2), timeout=1).close()
                return None
            except OSError:
                time.sleep(0.5)
        return f'{self.name} never opened its WebSocket port in {self.args.startup_timeout}s'

    def wait_log(self, text, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with open(self.logpath, errors='replace') as f:
                if text in f.read():
                    return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.5)
        return False

    def kill(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                os.kill(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            self.proc = None
            self.log.close()

    def detach(self):
        if self.loop:
            subprocess.call(['losetup', '-d', self.loop])
            self.loop = None

    def tail(self, n=15):
        try:
            with open(self.logpath, errors='replace') as f:
                return ''.join(f.readlines()[-n:])
        except OSError:
            return ''

    def url(self):
        return f'ws://127.0.0.1:{self.port + 2}/'


def client(srv):
    c = orly.connect(srv.url(), timeout=10, recv_timeout=120)
    c.new_session()
    return c


def put(c, pov, n, op_id):
    return c.call(pov, PKG, 'put_once', {'w': 0, 'n': n, 'e': 1}, op_id=op_id, op_retries=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(REPO_ROOT, '../out_orly/release')))
    ap.add_argument('--port', type=int, default=19950, help='master uses port..port+3, slave port+10..port+13')
    ap.add_argument('--volume-gb', type=int, default=3)
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--negative', choices=['none', 'noretry'], default=os.environ.get('NEGATIVE', 'none'))
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('failover_retry_test.py needs root (losetup); run tests/failover_retry_test.sh')
    work = tempfile.mkdtemp(prefix='failover862.')
    master = Server(args, work, 'master', args.port)
    slave = Server(args, work, 'slave', args.port + 10)
    failures = []
    finished = False

    def check(ok, what):
        log(f'  {"ok  " if ok else "FAIL"} {what}')
        if not ok:
            failures.append(what)

    try:
        log(f'failover retry test: negative={args.negative} work={work}')
        os.makedirs(f'{work}/packages')
        open(f'{work}/packages/__orly__', 'w').close()
        subprocess.check_call([os.path.join(args.orly_out, 'orly/orlyc'), '--skip-tests', '-o', work,
                               os.path.join(REPO_ROOT, 'tests/kill_campaign.orly')], cwd=work,
                              stdout=subprocess.DEVNULL)
        shutil.copy(f'{work}/{PKG}.1.so', f'{work}/packages/')

        master.create_volume()
        err = master.start('SOLO')
        if err:
            sys.exit(f'{err}\n{master.tail()}')
        with client(master) as c:
            c.install(PKG, 1)
            pov = c.new_pov(safe=True, shared=True)
            op_a = orly.new_op_id()
            put(c, pov, 1, op_a)
        log(f'master: wrote key 1 with id {op_a} into POV {pov}')

        slave.create_volume()
        err = slave.start('REPLICA', master_slave_port=master.port + 1)
        if err:
            sys.exit(f'{err}\n{slave.tail()}')
        joined_at = time.time()
        if not slave.wait_log('to [Slave]', 120):
            sys.exit(f'slave never reached Slave state\n{slave.tail()}')
        with client(slave) as c:
            c.install(PKG, 1)
        log('slave: joined')

        with client(master) as c:
            op_b = orly.new_op_id()
            first = put(c, pov, 2, op_b)
        log(f'master: wrote key 2 with id {op_b}, replied {first!r}')
        # Let the write reach the slave; the read on the promoted slave below checks that it did.
        time.sleep(5)

        master.kill()
        if not slave.wait_log('slave promoted to solo', 120):
            sys.exit(f'slave never promoted\n{slave.tail()}')
        log('slave: promoted')

        with client(slave) as c:
            vals = c.call(pov, PKG, 'vals_of', {'w': 0})
            count = c.call(pov, PKG, 'count_of', {'w': 0})
            check(vals == [1001, 2001] and count == 2,
                  f'read: keys 1 and 2 on the promoted slave, counter 2 (got {vals!r}, counter {count!r})')
            try:
                again = put(c, pov, 2, op_b)
                count = c.call(pov, PKG, 'count_of', {'w': 0})
                check(again == first and count == 2,
                      f'retry: id B answered with the saved outcome {first!r}, counter 2 (got {again!r}, counter {count!r})')
            except orly.OrlyError as ex:
                check(False, f'retry: id B ran again on the promoted slave: {ex}')
            try:
                put(c, pov, 1, op_a)
                check(False, 'join: id A, from before the slave joined, was accepted')
            except orly.OperationExpired:
                check(True, 'join: id A, from before the slave joined, refused as operation_expired')
            except orly.OrlyError as ex:
                check(False, f'join: id A, from before the slave joined, ran again: {ex}')
            wait = joined_at + JOIN_SKEW_S + 2 - time.time()
            if wait > 0:
                log(f'waiting {wait:.0f}s for the join floor to pass')
                time.sleep(wait)
            try:
                put(c, pov, 3, orly.new_op_id())
                count = c.call(pov, PKG, 'count_of', {'w': 0})
                check(count == 3, f'fresh: a new id past the join floor applies (counter {count!r})')
            except orly.OrlyError as ex:
                check(False, f'fresh: a new id past the join floor was refused: {ex}')
        finished = True
    finally:
        master.kill()
        slave.kill()
        master.detach()
        slave.detach()
        if failures or not finished:
            for srv in (master, slave):
                log(f'--- {srv.name} log tail ---\n{srv.tail(30)}')
        if args.keep or failures or not finished:
            log(f'work directory kept: {work}')
        else:
            shutil.rmtree(work, ignore_errors=True)
    if failures:
        log(f'FAILOVER RETRY: FAIL ({len(failures)} of 4 checks)')
        sys.exit(1)
    log('FAILOVER RETRY: OK')


if __name__ == '__main__':
    main()
