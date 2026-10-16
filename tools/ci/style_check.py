#!/usr/bin/env python3
"""Advisory style check: clang-format and clang-tidy on CHANGED LINES only.

Compares the working tree (HEAD) against the merge-base with --base and runs

  * git clang-format-18 --diff     (formatting, per .clang-format)
  * clang-tidy-diff-18.py -p1      (lint, per .clang-tidy)

so a PR is never asked to fix code it did not touch -- there is no mass
reformat (it would wreck git blame). Findings are printed as GitHub
annotations (::warning file=..,line=..::...) plus a readable summary. The
exit status is 0 unless the tooling itself breaks; findings never fail the
check. See CONTRIBUTING.md ("Code style").
"""

import argparse
import os
import re
import subprocess
import sys

SOURCE = re.compile(r"^(base|orly|jhm)/.*\.(cc|h)$")
# Mirrors root.jhm: C++23, repo-root include path, host build define.
COMPILE_FLAGS = ["-std=c++23", "-x", "c++", "-I.", "-DORLY_HOST"]
TIDY_LINE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<level>warning|error): (?P<msg>.*) \[(?P<check>[^\]]+)\]$")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def annotate(level, path, line, title, msg):
    msg = msg.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::{level} file={path},line={line},title={title}::{msg}")


def changed_files(merge_base):
    out = run(["git", "diff", "--name-only", "--diff-filter=ACMR",
               merge_base, "HEAD"], check=True).stdout.split("\n")
    return [p for p in out if SOURCE.match(p)]


def check_format(merge_base, files, binary):
    """Return the number of hunks clang-format would change."""
    res = run(["git", "clang-format-18", "--binary", binary, "--diff",
               "--extensions", "cc,h", merge_base, "--", *files])
    if res.returncode not in (0, 1):
        print(f"clang-format step failed (rc={res.returncode}):\n"
              f"{res.stdout}{res.stderr}", file=sys.stderr)
        return None
    diff = res.stdout
    if diff.startswith(("no modified files", "clang-format did not modify")):
        return 0
    hunks, path = 0, None
    for ln in diff.splitlines():
        if ln.startswith("+++ "):
            path = ln[4:].removeprefix("b/")
        m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)", ln)
        if m and path:
            hunks += 1
            annotate("warning", path, m.group(1), "clang-format",
                     "Changed lines differ from .clang-format. Run "
                     "`git clang-format-18 <base>` to fix.")
    if hunks:
        print("\n---- clang-format would change these lines ----")
        print(diff)
    return hunks


def check_tidy(merge_base, files, tidy, tidy_diff, jobs):
    """Return the number of clang-tidy warnings on changed lines."""
    patch = run(["git", "diff", "-U0", merge_base, "HEAD", "--", *files],
                check=True).stdout
    if not patch.strip():
        return 0
    res = run([tidy_diff, "-p1", "-quiet", "-j", str(jobs),
               "-clang-tidy-binary", tidy, "--", *COMPILE_FLAGS],
              input=patch)
    out = res.stdout + res.stderr
    seen, count = set(), 0
    for ln in out.splitlines():
        m = TIDY_LINE.match(ln)
        if not m:
            continue
        check = m["check"]
        # A missing generated header (nycr output) or a system header the
        # runner lacks is a build-environment gap, not a finding.
        if check == "clang-diagnostic-error":
            continue
        path = os.path.relpath(m["file"]) if os.path.isabs(m["file"]) else m["file"]
        key = (path, m["line"], check)
        if key in seen:
            continue
        seen.add(key)
        count += 1
        annotate("warning", path, m["line"], check, m["msg"])
        print(f"{path}:{m['line']}: {m['msg']} [{check}]")
    if res.returncode not in (0, 1) and count == 0 and not out.strip() == "":
        print(f"(clang-tidy-diff exited {res.returncode})", file=sys.stderr)
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", required=True, help="base commit (PR base sha)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--clang-format", default="clang-format-18")
    ap.add_argument("--clang-tidy", default="clang-tidy-18")
    ap.add_argument("--clang-tidy-diff", default="clang-tidy-diff-18.py")
    ap.add_argument("--skip-tidy", action="store_true")
    args = ap.parse_args()

    merge_base = run(["git", "merge-base", args.base, "HEAD"],
                     check=True).stdout.strip()
    files = changed_files(merge_base)
    print(f"merge-base {merge_base[:12]}; {len(files)} changed C++ file(s)")
    for f in files:
        print(f"  {f}")
    if not files:
        print("Nothing to check.")
        return 0

    fmt = check_format(merge_base, files, args.clang_format)
    tidy = 0 if args.skip_tidy else check_tidy(
        merge_base, files, args.clang_tidy, args.clang_tidy_diff, args.jobs)

    print(f"\nclang-format: {'tool error' if fmt is None else f'{fmt} hunk(s)'}"
          f"; clang-tidy: {tidy} finding(s) (advisory, never fails the job)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
