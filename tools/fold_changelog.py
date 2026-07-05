#!/usr/bin/env python3
"""Fold changelog.d/ fragments into CHANGELOG.md's [Unreleased] section."""
import os
import sys

FRAG_DIR = 'changelog.d'
MARKER = '## [Unreleased]\n\n### Fixed\n'

def main():
    frags = [
        os.path.join(FRAG_DIR, name)
        for name in os.listdir(FRAG_DIR)
        if name.endswith('.md') and name != 'README.md'
    ]
    if not frags:
        print('no fragments to fold')
        return 0
    frags.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    entries = []
    for path in frags:
        with open(path) as f:
            text = f.read().strip()
        if not text.startswith('- **'):
            sys.exit(f'{path}: fragment must start with a "- **" bullet')
        entries.append(text + '\n')
    with open('CHANGELOG.md') as f:
        changelog = f.read()
    if MARKER not in changelog:
        sys.exit('CHANGELOG.md: cannot find the [Unreleased] ### Fixed marker')
    changelog = changelog.replace(MARKER, MARKER + ''.join(entries), 1)
    with open('CHANGELOG.md', 'w') as f:
        f.write(changelog)
    for path in frags:
        os.remove(path)
    print(f'folded {len(frags)} fragment(s)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
