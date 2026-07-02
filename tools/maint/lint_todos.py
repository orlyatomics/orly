#!/usr/bin/env python3
"""Lint the TODO convention so 2014-era noise can't creep back.

Every `TODO` marker in tracked C/C++ sources must reference a tracked issue in
the form `TODO(#<issue>)`. Two failure modes are reported:

  1. Bare `/* TODO */` (or `/* TODO. */`) doc-stubs -- the empty 2014
     placeholders stripped in #271/#274. These carry no information; write a
     real doc-comment or delete the line.
  2. Any other `TODO` not written `TODO(#<issue>)` -- an untracked task. File
     an issue and reference it, so the backlog lives in the tracker, not in
     scattered comments.

The whole texted-TODO backlog was converted to `TODO(#nnn)` in the phase-1/3
cleanup, so the conforming count is total and this lint keeps it that way.

Scope: tracked `*.h *.cc *.cpp *.hpp` (matches strip_todo_stubs.py and the
house doc-comment style). This file and strip_todo_stubs.py are excluded
(they contain the literal token `TODO` as data, not as a task marker).

Usage:
  lint_todos.py            # check the whole tree (CI default)
  lint_todos.py --base REF # only check TODOs added since REF (fast PR path)
"""
import re
import subprocess
import sys

EXTS = ('*.h', '*.cc', '*.cpp', '*.hpp')

EXCLUDE = {
    'tools/maint/strip_todo_stubs.py',
    'tools/maint/lint_todos.py',
}

# Whole-line bare stub: optional indent, /* TODO */ or /* TODO. */, nothing else.
BARE = re.compile(r'^[ \t]*/\*\s*TODO\.?\s*\*/[ \t]*$')
# A conforming reference: TODO immediately followed by (#<digits>).
CONFORMING = re.compile(r'TODO\(#\d+\)')
# Any TODO task marker (uppercase house style; lowercase "todo" in identifiers
# is not a marker and is ignored).
ANY_TODO = re.compile(r'TODO')


def tracked_sources():
    out = subprocess.check_output(['git', 'ls-files', *EXTS], text=True)
    return [p for p in out.splitlines() if p and p not in EXCLUDE]


def nonconforming_in(text):
    """True if `text` contains a TODO marker not written TODO(#nnn)."""
    return bool(ANY_TODO.search(CONFORMING.sub('', text)))


def scan_tree():
    """Return (bare, untracked) violation lists over the whole tree."""
    bare, untracked = [], []
    for path in tracked_sources():
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for n, line in enumerate(f, 1):
                    if BARE.match(line.rstrip('\n')):
                        bare.append((path, n, line.strip()))
                    elif nonconforming_in(line):
                        untracked.append((path, n, line.strip()))
        except (UnicodeDecodeError, FileNotFoundError):
            continue
    return bare, untracked


def scan_diff(base):
    """Return (bare, untracked) violations among lines added since `base`."""
    diff = subprocess.check_output(
        ['git', 'diff', '--unified=0', f'{base}...HEAD', '--', *EXTS], text=True)
    path = None
    bare, untracked = [], []
    for line in diff.splitlines():
        if line.startswith('+++ b/'):
            path = line[6:]
        elif line.startswith('+') and not line.startswith('+++') and path \
                and path not in EXCLUDE:
            added = line[1:]
            if BARE.match(added.rstrip('\n')):
                bare.append((path, '+', added.strip()))
            elif nonconforming_in(added):
                untracked.append((path, '+', added.strip()))
    return bare, untracked


def main():
    base = None
    if '--base' in sys.argv:
        base = sys.argv[sys.argv.index('--base') + 1]

    bare, untracked = scan_diff(base) if base else scan_tree()
    failed = False

    if bare:
        failed = True
        print(f'ERROR: {len(bare)} bare /* TODO */ stub(s) '
              '(write a real doc-comment or delete the line):')
        for path, n, text in bare:
            print(f'  {path}:{n}: {text}')

    if untracked:
        failed = True
        print(f'\nERROR: {len(untracked)} TODO(s) not in TODO(#nnn) form '
              '(file an issue and reference it):')
        for path, n, text in untracked:
            print(f'  {path}:{n}: {text}')

    if failed:
        print('\nTODO lint failed. See CONTRIBUTING.md for the convention.')
        return 1
    scope = f'new TODOs since {base}' if base else 'all tracked TODOs'
    print(f'TODO lint passed ({scope} reference a tracked issue).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
