#!/usr/bin/env python3
"""Paired A/B benchmark: run one command against two builds on the same machine.

Usage: ab_bench.py --a OUT_A --b OUT_B [--rounds N] [--label-a A] [--label-b B] -- CMD...

OUT_A and OUT_B are release output trees (the directory holding orly/server/orlyi
and orly/orlyc). CMD runs once per arm per round, with ORLY_OUT pointing at that
arm's tree, so both arms run the same harness and workload and only the binaries
differ. The arms alternate, and the order flips each round (AB, BA, AB, ...), so
slow drift on the machine (thermal, a noisy neighbour) lands on both arms equally.

CMD reports results as lines on stdout or stderr:

    METRIC <name> <number>

For each metric, the report gives each arm's median, min and max, B's median over
A's, and the median of the per-round B/A ratios. The paired ratio is the number
to trust: it compares runs taken back to back. Comparing runs from different
machines is what made #587 look 14% slower when it wasn't.

A run that exits non-zero is reported with the tail of its output and left out
of the statistics, and the script exits 1 at the end. Everything else exits 0;
this measures, it doesn't judge. Full output of every run goes to --log-dir.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys

METRIC = re.compile(r"^METRIC\s+(\S+)\s+(-?[0-9.]+(?:[eE][-+]?[0-9]+)?)\s*$")


def run(cmd, out_dir, log_path):
    env = dict(os.environ, ORLY_OUT=out_dir)
    with open(log_path, "w") as log:
        proc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    metrics = {}
    with open(log_path, errors="replace") as log:
        for line in log:
            m = METRIC.match(line.strip())
            if m:
                metrics[m.group(1)] = float(m.group(2))
    return proc.returncode, metrics


def fmt(x):
    return f"{x:,.1f}" if abs(x) < 1e6 else f"{x:,.0f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--a", required=True, help="release output tree of arm A")
    ap.add_argument("--b", required=True, help="release output tree of arm B")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--log-dir", default="ab-bench-logs")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        ap.error("give the command to run after --")
    if args.rounds < 1:
        ap.error("--rounds must be at least 1")
    os.makedirs(args.log_dir, exist_ok=True)

    arms = {"A": os.path.abspath(args.a), "B": os.path.abspath(args.b)}
    label = {"A": args.label_a, "B": args.label_b}
    results = {"A": [], "B": []}  # per round: metrics dict, or None if the run failed
    failed = []
    for r in range(1, args.rounds + 1):
        for arm in ("AB" if r % 2 else "BA"):
            log_path = os.path.join(args.log_dir, f"round{r}-{arm}.log")
            rc, metrics = run(cmd, arms[arm], log_path)
            shown = " ".join(f"{k}={fmt(v)}" for k, v in sorted(metrics.items()))
            print(f"round {r} {arm} ({label[arm]}): rc={rc} {shown}", flush=True)
            if rc != 0:
                failed.append((r, arm, log_path))
                results[arm].append(None)
            else:
                results[arm].append(metrics)

    names = sorted({k for arm in "AB" for m in results[arm] if m for k in m})
    print()
    print(f"A = {label['A']}  ({arms['A']})")
    print(f"B = {label['B']}  ({arms['B']})")
    print(f"{args.rounds} rounds, order alternating AB/BA; {len(failed)} failed run(s)")
    print()
    if not names:
        print("No METRIC lines in any successful run; nothing to compare.")
    else:
        print("| metric | A median (min..max) | B median (min..max) | B/A medians | B/A paired (min..max) | pairs |")
        print("| -- | -- | -- | -- | -- | -- |")
        for name in names:
            a = [m[name] for m in results["A"] if m and name in m]
            b = [m[name] for m in results["B"] if m and name in m]
            pairs = [mb[name] / ma[name]
                     for ma, mb in zip(results["A"], results["B"])
                     if ma and mb and name in ma and name in mb and ma[name] != 0]
            def col(xs):
                return f"{fmt(statistics.median(xs))} ({fmt(min(xs))}..{fmt(max(xs))})" if xs else "-"
            ratio = (f"{statistics.median(b) / statistics.median(a):.3f}"
                     if a and b and statistics.median(a) != 0 else "-")
            paired = (f"{statistics.median(pairs):.3f} ({min(pairs):.3f}..{max(pairs):.3f})"
                      if pairs else "-")
            print(f"| {name} | {col(a)} | {col(b)} | {ratio} | {paired} | {len(pairs)} |")
    for r, arm, log_path in failed:
        print(f"\nFAILED: round {r} arm {arm} ({label[arm]}), last lines of {log_path}:")
        with open(log_path, errors="replace") as log:
            for line in log.readlines()[-40:]:
                print("  " + line.rstrip())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
