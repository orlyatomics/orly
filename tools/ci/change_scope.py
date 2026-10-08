#!/usr/bin/env python3
"""Classify a tested PR merge conservatively; uncertainty means full CI."""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess


SAFE_DOCS = {
    "README.md", "CONTRIBUTING.md", "CHANGELOG.md",
    "docs/architecture.md", "docs/walkthrough.md",
}
FRAGMENT = re.compile(r"changelog\.d/[A-Za-z0-9][A-Za-z0-9_-]*\.md")
SHA = re.compile(r"[0-9a-f]{40}")


def full(reason):
    return {"scope": "full", "docs_only": False, "reason": reason}


def safe_path(path):
    return (
        bool(path) and not PurePosixPath(path).is_absolute()
        and "\\" not in path
        and all(part not in ("", ".", "..") for part in path.split("/"))
        and not any(char in path for char in ("\n", "\r"))
    )


def parse_diff(data):
    if not data:
        return []
    if not data.endswith(b"\0"):
        raise ValueError("unterminated changed-file list")
    tokens = data.split(b"\0")[:-1]
    changes = []
    index = 0
    while index < len(tokens):
        status = tokens[index].decode("ascii")
        index += 1
        if status in ("A", "D", "M"):
            count = 1
        elif re.fullmatch(r"[RC][0-9]{1,3}", status) and int(status[1:]) <= 100:
            count = 2
        else:
            raise ValueError(f"unrecognized change status {status}")
        if len(tokens) - index < count:
            raise ValueError("incomplete changed-file record")
        paths = tuple(token.decode("utf-8") for token in tokens[index:index + count])
        if not all(safe_path(path) for path in paths):
            raise ValueError("invalid changed-file path")
        changes.append((status[0], paths))
        index += count
    return changes


def classify(changes):
    paths = {path for _, names in changes for path in names}
    if not paths:
        return full("empty diff")
    if all(path in SAFE_DOCS or FRAGMENT.fullmatch(path) for path in paths):
        return {"scope": "docs-only", "docs_only": True, "reason": "all paths are explicitly allowlisted documentation"}
    if all(path.startswith("clients/") for path in paths):
        return {"scope": "clients-only", "docs_only": False, "reason": "client changes retain all native and integration coverage"}
    return full("non-allowlisted or mixed paths")


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], timeout=60)


def tree_modes(repo, commit, paths):
    modes = {}
    output = git(repo, "ls-tree", "-z", commit, "--", *sorted(paths))
    for record in output.split(b"\0"):
        if not record:
            continue
        info, path = record.split(b"\t", 1)
        mode, kind, _ = info.decode("ascii").split()
        if kind != "blob":
            raise ValueError("documentation path is not a blob")
        modes[path.decode("utf-8")] = mode
    return modes


def regular_documentation(repo, base, tested, changes):
    paths = {path for _, names in changes for path in names}
    before, after = tree_modes(repo, base, paths), tree_modes(repo, tested, paths)
    for status, names in changes:
        old = names[0]
        new = names[-1]
        if status != "A" and before.get(old) != "100644":
            return False
        if status != "D" and after.get(new) != "100644":
            return False
    return True


def classify_repository(repo, base, tested, event):
    if event != "pull_request":
        return full("non-PR trigger")
    if not base or not tested or not SHA.fullmatch(base) or not SHA.fullmatch(tested):
        return full("missing or invalid tested/base SHA")
    try:
        head = git(repo, "rev-parse", "HEAD").decode().strip()
        parents = git(repo, "rev-list", "--parents", "-n", "1", tested).decode().split()
        if head != tested or len(parents) != 3 or parents[1] != base:
            return full("checkout/base does not match the tested two-parent merge")
        changes = parse_diff(git(repo, "diff", "--name-status", "-z", "--find-renames", "--find-copies", base, tested, "--"))
        result = classify(changes)
        if result["docs_only"] and not regular_documentation(repo, base, tested, changes):
            return full("documentation mode/type is not a regular non-executable file")
        return result
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError) as error:
        return full(f"diff classification unavailable: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--base")
    parser.add_argument("--tested")
    parser.add_argument("--event", required=True)
    args = parser.parse_args()
    result = classify_repository(args.repo, args.base, args.tested, args.event)
    if "GITHUB_OUTPUT" in os.environ:
        with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
            output.write(f"docs_only={str(result['docs_only']).lower()}\nscope={result['scope']}\n")
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as output:
            output.write(f"### CI scope: {result['scope']}\n\n{result['reason']}\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
