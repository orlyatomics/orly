#!/usr/bin/env python3
"""Strip lines that are EXACTLY a bare `/* TODO */` doc-stub placeholder.

Only whole-line bare stubs are removed (leading/trailing whitespace allowed).
Anything with text -- `/* TODO: ... */`, `// TODO foo`, a trailing `/* TODO */`
after code -- is left untouched. Removing a whole-line comment can never change
compilation, so this is a safe, mechanical noise cleanup.
"""
import re
import subprocess
import sys

# Exact bare block-comment stub on its own line: optional indent, /* TODO */,
# optional trailing space. The `\s*\*/` right after TODO means a `: text` body
# can't match.
BARE = re.compile(r'^[ \t]*/\*\s*TODO\s*\*/[ \t]*$')

EXCLUDE = {
    'orly/indy/repo.h',  # handled by the doc-comment pilot (disjoint change)
}

def tracked_sources():
    out = subprocess.check_output(
        ['git', 'ls-files', '*.h', '*.cc', '*.cpp', '*.hpp'],
        text=True,
    )
    return [p for p in out.splitlines() if p and p not in EXCLUDE]

def main():
    apply = '--apply' in sys.argv
    total_lines = 0
    changed_files = 0
    per_file = []
    for path in tracked_sources():
        try:
            with open(path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
        except (UnicodeDecodeError, FileNotFoundError):
            continue
        kept = [ln for ln in lines if not BARE.match(ln.rstrip('\n'))]
        removed = len(lines) - len(kept)
        if removed:
            changed_files += 1
            total_lines += removed
            per_file.append((removed, path))
            if apply:
                with open(path, 'w', encoding='utf-8') as f:
                    f.writelines(kept)
    per_file.sort(reverse=True)
    print(f"{'APPLIED' if apply else 'DRY-RUN'}: "
          f"{total_lines} bare /* TODO */ stub lines across {changed_files} files")
    print("top 15 files:")
    for removed, path in per_file[:15]:
        print(f"  {removed:5d}  {path}")

if __name__ == '__main__':
    main()
