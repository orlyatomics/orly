#!/usr/bin/env python3
"""Fail on merge-conflict markers left in tracked text files (#734).

PRs here are rebased by tools and by hand, and a leftover marker passes CI
unless it happens to break a build or a test. Docs, CHANGELOG.md,
changelog.d/ fragments and shell scripts are the likely places.

Flagged, as git writes them (at the very start of a line):
  <<<<<<<            alone or followed by a space and a label
  >>>>>>>            alone or followed by a space and a label
  =======            alone, ONLY between a <<<<<<< and a >>>>>>> in the same
                     file. A bare line of seven = signs is otherwise legal
                     (markdown setext headings, separator comments), so it
                     is never flagged on its own.

Every violation is reported as file:line. Binary files (any NUL byte) are
skipped.

Allowlist: a doc that legitimately SHOWS a conflict goes in ALLOWLIST below,
as `path: reason`. A reason is mandatory; an empty one is itself an error.
No file needs it today.

Usage:
  lint_conflict_markers.py     # check every tracked file (CI default)
"""
import re
import subprocess
import sys

ALLOWLIST = {
    # 'docs/example.md': 'shows what a conflict looks like to the reader',
}

OPEN = re.compile(r'^<{7}( |$)')
CLOSE = re.compile(r'^>{7}( |$)')
MID = re.compile(r'^={7}$')


def scan(text):
    """Return [(lineno, line)] for each conflict marker in `text`."""
    hits = []
    pending_mid = []  # '=======' lines seen since the last <<<<<<<
    in_conflict = False
    for n, line in enumerate(text.splitlines(), 1):
        if OPEN.match(line):
            hits.append((n, line))
            in_conflict, pending_mid = True, []
        elif CLOSE.match(line):
            hits.append((n, line))
            hits.extend(pending_mid)
            in_conflict, pending_mid = False, []
        elif in_conflict and MID.match(line):
            pending_mid.append((n, line))
    return sorted(hits)


def main():
    bad = [p for p, why in ALLOWLIST.items() if not why.strip()]
    for p in bad:
        print(f'{p}: allowlist entry has no reason', file=sys.stderr)
    if bad:
        return 1
    paths = subprocess.check_output(['git', 'ls-files', '-z'], text=True).split('\0')
    found = 0
    for path in filter(None, paths):
        if path in ALLOWLIST:
            continue
        try:
            with open(path, 'rb') as f:
                raw = f.read()
        except OSError:
            continue  # submodule, dangling symlink, deleted in the worktree
        if b'\0' in raw:
            continue
        for n, line in scan(raw.decode('utf-8', 'replace')):
            print(f'{path}:{n}: merge-conflict marker: {line.rstrip()}')
            found += 1
    if found:
        print(f'\n{found} conflict marker(s). Resolve them; if a doc must show '
              'one, add it to ALLOWLIST in tools/maint/lint_conflict_markers.py '
              'with a reason.', file=sys.stderr)
        return 1
    print('no merge-conflict markers')
    return 0


if __name__ == '__main__':
    sys.exit(main())
