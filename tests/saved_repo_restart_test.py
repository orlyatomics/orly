#!/usr/bin/env python3
"""A restart keeps the saved-repo entries of the safe POVs that WAL recovery restores (#966).

Every repo but the system repo has an entry in the system repo, and a restart has the next repo
creation remove the entries of repos that did not survive it (#671). The sweep noted them before
WAL recovery, so the safe POVs recovery was about to restore were among them: the first restored
POV's creation removed the others' entries, and each restore wrote its own back. On an image with
2,116 open safe POVs every open removed and rewrote the same ~2,100 entries.

On a fresh loopback volume (as tests/pause_restart_test.py) it makes a few safe POVs and a fast
one, then stops and opens the store twice, each time making one new POV so the removals are
written. On each open it requires:

  - recovery to restore the safe POVs, and each to answer a review;
  - none of their saved-repo entries to be removed;
  - on the first open, the fast POV's entry to be removed (it did not survive);
  - on the second, at most one entry of a repo that is gone to come back: the fast POV the
    first open made, which the second must remove.

Needs root (losetup); run it through tests/saved_repo_restart_test.sh. It ends with one summary
line starting "SAVED REPO RESTART:" and exits nonzero on any violation.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pause_restart_test import Server, log, orly  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
RESTORED = re.compile(r'WAL recovery complete: .*\((?:head \d+, )?(\d+) povs restored\)')
CAME_BACK = re.compile(r'\[(\d+)\] saved-repo entries came back with the system repo(?:, \[(\d+)\] of them of live repos)?')
REMOVED = re.compile(r'TManager: removed the saved entry of repo \[([0-9a-f-]+)\]')


def wait_removals(srv, timeout):
    """The ids whose saved entries this run removed, once the count stops changing."""
    deadline = time.monotonic() + timeout
    last = None
    while True:
        got = set(REMOVED.findall(srv.log_text()))
        if got == last or time.monotonic() >= deadline:
            return got
        last = got
        time.sleep(1)


def reopen(args, srv, url, sid, kept, label):
    """Start the store again, make one POV so the queued removals are written, and check what
    came back. Returns (violations, the new POV's id, how many entries of repos that are gone
    came back, the ids whose entries were removed)."""
    kv = []
    err = srv.start(create=False)
    if err:
        return [f'{label}: start: {err}\n{srv.tail()}'], None, 0, set()
    with orly.connect(url, timeout=10, recv_timeout=120) as c:
        c.resume_session(sid)
        for pov in kept:
            try:
                c.review(pov)
            except orly.OrlyError as ex:
                kv.append(f'{label}: safe POV {pov} did not come back: {ex}')
        # Fast, so no Pov record brings it back: the next open must remove its entry.
        made = c.new_pov(safe=False, shared=True)
    removed = wait_removals(srv, args.settle)
    text = srv.log_text()
    m = RESTORED.search(text)
    restored = int(m.group(1)) if m else -1
    if restored < len(kept):
        kv.append(f'{label}: recovery restored {restored} POVs, fewer than the {len(kept)} kept')
    lost = sorted(set(kept) & removed)
    if lost:
        kv.append(f'{label}: the saved entries of {len(lost)} of the {len(kept)} restored safe POVs were removed')
    m = CAME_BACK.search(text)
    total = int(m.group(1)) if m else -1
    live = int(m.group(2)) if m and m.group(2) else 0
    log(f'  {label}: {restored} POVs restored; {total} saved-repo entries came back, {live} of live repos; '
        f'{len(removed)} removed, {len(lost)} of them restored POVs\'')
    return kv, made, total - live, removed


def run(args, work):
    srv = Server(args, work, 'saved966')
    url = f'ws://127.0.0.1:{args.port + 2}/'
    kv = []
    try:
        srv.create_volume()
        err = srv.start(create=True)
        if err:
            return [f'create: {err}\n{srv.tail()}']
        srv.stop()
        err = srv.start(create=False)
        if err:
            return [f'first restart: {err}\n{srv.tail()}']
        with orly.connect(url, timeout=10, recv_timeout=120) as c:
            sid = c.new_session()
            kept = [c.new_pov(safe=True, shared=True) for _ in range(args.povs)]
            fast = c.new_pov(safe=False, shared=True)
        srv.stop()

        got, made, gone, removed = reopen(args, srv, url, sid, kept, 'open 1')
        kv += got
        if fast not in removed:
            kv.append(f'open 1: the fast POV {fast}, which did not survive the restart, kept its saved entry')
        srv.stop()

        got, _, gone, removed = reopen(args, srv, url, sid, kept, 'open 2')
        kv += got
        # Only the POV open 1 made is gone since.
        if gone > 1:
            kv.append(f'open 2: {gone} saved-repo entries of repos that are gone came back, not at most 1')
        if made not in removed:
            kv.append(f'open 2: the POV open 1 made, {made}, kept its saved entry')
        if re.search(r'FATAL ERROR|TERMINATE', srv.log_text()):
            kv.append('the log shows a FATAL ERROR or TERMINATE')
        srv.stop()
        return kv
    finally:
        if srv.proc is not None:
            srv.kill()
        srv.detach()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--orly-out', default=os.environ.get('ORLY_OUT', os.path.join(REPO_ROOT, '../out_orly/debug')))
    ap.add_argument('--orlyi-prefix', default=os.environ.get('ORLYI_PREFIX', ''))
    ap.add_argument('--extra-args', default=os.environ.get('EXTRA_ARGS', ''),
                    help='more orlyi flags; one named here replaces the default of the same name')
    ap.add_argument('--port', type=int, default=19980)
    ap.add_argument('--povs', type=int, default=8, help='safe POVs to keep across the restarts')
    ap.add_argument('--volume-gb', type=int, default=2)
    ap.add_argument('--wal-mb', type=int, default=8)
    ap.add_argument('--settle', type=float, default=5, help='seconds to wait for the removals')
    ap.add_argument('--startup-timeout', type=int, default=300)
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    args.orly_out = os.path.abspath(args.orly_out)
    if os.geteuid() != 0:
        sys.exit('saved_repo_restart_test.py needs root (losetup); run tests/saved_repo_restart_test.sh')
    work = tempfile.mkdtemp(prefix='saved966.')
    os.makedirs(f'{work}/packages')
    open(f'{work}/packages/__orly__', 'w').close()
    began = time.monotonic()
    try:
        kv = run(args, work)
    except Exception as ex:  # noqa: BLE001
        kv = [f'{type(ex).__name__}: {ex}']
    for v in kv:
        log(f'  VIOLATION: {v}')
    if not args.keep and not kv:
        shutil.rmtree(work, ignore_errors=True)
    else:
        log(f'work directory kept: {work}')
    log(f'SAVED REPO RESTART: povs={args.povs} violations={len(kv)} ({time.monotonic() - began:.0f}s)')
    sys.exit(1 if kv else 0)


if __name__ == '__main__':
    main()
