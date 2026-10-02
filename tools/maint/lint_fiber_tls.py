#!/usr/bin/env python3
"""Lint thread-locals so the #554/#578 fiber hazard can't come back unseen.

Orly's fibers migrate OS threads, and GCC computes the thread pointer once per
function, so a bare `__thread` / `thread_local` read taken after a fiber switch
can return the PREVIOUS thread's value. On aarch64 that hoist is real (x86-64
re-derives the TLS base at every access, which is why amd64 never shows it), and
only in optimised builds, so neither the amd64 jobs nor the debug arm job see it.
#554 (orlyc hang) and #578 (arm64 segfault on first write) were both this.

The fix is `Fiber::TFiberSafeLocal` (orly/indy/fiber/fiber.h), whose accessors
are opaque calls the optimiser must re-make at every access. This lint fails on
any bare thread-local in a tracked C/C++ source that is not on the allowlist
below, so a new one has to either use TFiberSafeLocal or be added here with a
reason it can never be read across a fiber switch.

Comments are stripped first, so prose mentioning `__thread` does not trip it.

Usage:
  lint_fiber_tls.py        # check the whole tree (CI default)
"""
import re
import subprocess
import sys

EXTS = ('*.h', '*.cc', '*.cpp', '*.hpp')

# (path, name) -> why it is safe. `name` is the declared identifier.
ALLOWED = {
    ('orly/indy/fiber/fiber.h', 'Slot'):
        'TFiberSafeLocal itself: touched only by its noipa Get()/Set()',
    ('orly/indy/disk/read_file.h', 'HashHitCount'):
        'statistics counter; a wrong-thread increment miscounts, cannot crash',
    ('orly/indy/disk/read_file.cc', 'HashHitCount'):
        'definitions of the counter above',
    ('base/thread_local_sigma_calc.h', 'registry'):
        'statistics only; if inlined across a fiber switch it can attribute a '
        'sample to the wrong thread\'s registry, never dereference null',
    ('orly/synth/type_def.cc', 'InFlightStack'):
        'compiler (orlyc front end), single-threaded, no fibers',
    ('orly/synth/type_def.cc', 'CurrentScc'):
        'compiler (orlyc front end), single-threaded, no fibers',
    ('orly/client/client.h', 'DispatchSelfDestructed'):
        'client dispatch thread, plain std::thread, no fibers',
    ('orly/client/client.cc', 'DispatchSelfDestructed'):
        'definition of the flag above',
}

COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
STRINGS = re.compile(r'"(?:\\.|[^"\\\n])*"')
# A declaration: the keyword, then everything up to the declared name, which is
# the last identifier before `;`, `=`, `[` or `{` (definitions qualify it with
# `Class::`, so take the part after the final `::`).
DECL = re.compile(r'\b(__thread|thread_local)\b([^;={\[]*)')
IDENT = re.compile(r'[A-Za-z_]\w*')


def tracked_sources():
    out = subprocess.check_output(['git', 'ls-files', *EXTS], text=True)
    return [p for p in out.splitlines() if p and '.test.' not in p]


def strip(text):
    # Keep newlines so reported line numbers stay right.
    blank = lambda m: re.sub(r'[^\n]', ' ', m.group(0))
    return COMMENTS.sub(blank, STRINGS.sub(blank, text))


def declarations(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        text = strip(f.read())
    for m in DECL.finditer(text):
        names = IDENT.findall(m.group(2).split('::')[-1])
        name = names[-1] if names else '?'
        yield text.count('\n', 0, m.start()) + 1, name


def main():
    bad = []
    for path in tracked_sources():
        for line, name in declarations(path):
            if (path, name) not in ALLOWED:
                bad.append(f'{path}:{line}: bare thread-local `{name}`')
    if bad:
        print('Bare thread-locals found. A fiber that migrates OS threads can '
              'read another thread\'s value through these on aarch64 release '
              'builds (#554, #578). Use Fiber::TFiberSafeLocal, or add the '
              'variable to ALLOWED in tools/maint/lint_fiber_tls.py with the '
              'reason it is never read across a fiber switch.\n')
        print('\n'.join(bad))
        return 1
    print(f'fiber TLS lint: ok ({len(ALLOWED)} allowlisted)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
