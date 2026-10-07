#!/usr/bin/env python3
"""Collect release measurements, compare paired runs, and render the published tables."""

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TAG = re.compile(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?")
START = "<!-- benchmark-results:start -->"
END = "<!-- benchmark-results:end -->"


def version_key(version):
    match = TAG.fullmatch(version)
    if not match:
        raise ValueError(f"not a release version: {version!r}; use vMAJOR.MINOR.PATCH[-PRERELEASE]")
    major, minor, patch, pre = match.groups()
    identifiers = (pre or "").split(".")
    if pre and any(not item or (item.isdigit() and len(item) > 1 and item[0] == "0")
                   for item in identifiers):
        raise ValueError(f"invalid prerelease version: {version!r}")
    suffix = tuple((0, int(item)) if item.isdigit() else (1, item) for item in identifiers)
    return int(major), int(minor), int(patch), pre is None, suffix


def load_suite():
    path = ROOT / "bench/suite.json"
    suite = json.loads(path.read_text())
    if suite["rounds"] < 3 or suite["timeout_seconds"] <= 0:
        raise ValueError("the release suite needs at least three rounds and a positive deadline")
    digest = hashlib.sha256(path.read_bytes())
    seen = set()
    for workload in suite["workloads"]:
        name = workload["id"]
        if name in seen or not re.fullmatch(r"[a-z0-9-]+", name):
            raise ValueError(f"invalid or duplicate workload: {name!r}")
        seen.add(name)
        if not workload["headline"] or any(direction not in ("higher", "lower")
                                           for direction in workload["headline"].values()):
            raise ValueError(f"{name}: declare the direction of every headline metric")
        for source in workload["inputs"]:
            digest.update(source.encode() + b"\0" + (ROOT / source).read_bytes())
    return suite, digest.hexdigest()


def history(directory):
    records = []
    for path in sorted(Path(directory).glob("*.json")):
        record = json.loads(path.read_text())
        version_key(record["version"])
        if record["schema_version"] != 1 or not record["complete"]:
            raise ValueError(f"invalid published measurement: {path}")
        if path.name != f"{record['version']}.json":
            raise ValueError(f"measurement version does not match its filename: {path}")
        records.append(record)
    return sorted(records, key=lambda record: version_key(record["version"]))


def make_plan(version, records, suite, fingerprint, publish=False):
    if version:
        version_key(version)
        ref = f"refs/tags/{version}^{{commit}}"
    else:
        if publish:
            raise ValueError("publication requires an existing release tag")
        ref = "HEAD"
    commit = subprocess.check_output(["git", "rev-parse", "--verify", ref], cwd=ROOT, text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("release ref did not resolve to a commit")
    version = version or f"dev-{commit[:12]}"
    older = [record for record in records
             if version.startswith("dev-") or version_key(record["version"]) < version_key(version)]
    previous = older[-1] if older else None
    state = "first_measurement"
    baseline = None
    if previous:
        state = "compatible"
        if previous["suite_fingerprint"] != fingerprint:
            state = "suite_changed"
        elif previous["runner"]["type"] != suite["runner"]:
            state = "runner_changed"
        if state == "compatible":
            if not re.fullmatch(r"[0-9a-f]{40}", previous["commit"]):
                raise ValueError("published baseline has an invalid commit")
            baseline = {"version": previous["version"], "commit": previous["commit"]}
    harness_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    return {"version": version, "commit": commit, "harness_commit": harness_commit, "baseline": baseline,
            "baseline_state": state, "suite_fingerprint": fingerprint,
            "previous_published_version": previous["version"] if previous else None}


def summary(values):
    return {"samples": values, "n": len(values), "median": statistics.median(values),
            "min": min(values), "max": max(values)} if values else None


def paired(report, name):
    rounds = {}
    for run in report["runs"]:
        if run["returncode"] == 0:
            rounds.setdefault(run["round"], {})[run["arm"]] = run["metrics"]
    ratios = []
    for arms in rounds.values():
        a, b = arms.get("A", {}).get(name), arms.get("B", {}).get(name)
        if a is not None and b is not None and a > 0 and b > 0:
            ratios.append(b / a)
    return summary(ratios)


def check_runs(report, headline, rounds):
    errors = []
    expected = {(r, arm) for r in range(1, rounds + 1) for arm in "AB"}
    actual = [(run["round"], run["arm"]) for run in report["runs"]]
    if set(actual) != expected or len(actual) != len(expected):
        errors.append("missing or duplicate arm/round")
    for run in report["runs"]:
        label = f"round {run['round']} {run['arm']}"
        if run["returncode"] != 0:
            errors.append(f"{label}: exit {run['returncode']} (see {run['log']})")
        for name, value in run["metrics"].items():
            if not isinstance(value, (int, float)) or not math.isfinite(value):
                errors.append(f"{label}: invalid METRIC {name}={value!r}")
        for name in headline:
            value = run["metrics"].get(name)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                errors.append(f"{label}: missing or nonpositive headline METRIC {name}")
    return errors


def measure(workload, phase, a, b, rounds, deadline, output):
    directory = output / "logs" / workload["id"] / phase
    directory.mkdir(parents=True)
    data = directory / "runs.json"
    command = [sys.executable, str(ROOT / "tools/maint/ab_bench.py"),
               "--a", str(a), "--b", str(b), "--rounds", str(rounds),
               "--timeout", str(deadline), "--fail-fast", "--log-dir", str(directory),
               "--json", str(data), "--"] + workload["command"]
    with (directory / "report.txt").open("w") as log:
        proc = subprocess.run(command, cwd=ROOT, env=dict(os.environ, **workload["env"]),
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        report = json.loads(data.read_text())
    except (OSError, ValueError) as err:
        raise ValueError(f"{workload['id']} {phase}: exit {proc.returncode}, no valid runs.json; "
                         f"see {directory / 'report.txt'}") from err
    for run in report["runs"]:
        run["log"] = str(Path(run["log"]).relative_to(output))
    errors = check_runs(report, workload["headline"], rounds)
    if proc.returncode and not errors:
        errors.append(f"paired collector exited {proc.returncode}")
    return report, errors


def runner_info(runner):
    cpu = platform.processor() or platform.machine()
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        cpu = next((line.split(":", 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                    if line.startswith("model name")), cpu)
    memory = Path("/proc/meminfo")
    ram = next((int(line.split()[1]) for line in memory.read_text().splitlines()
                if line.startswith("MemTotal:")), None) if memory.exists() else None
    return {"type": runner, "architecture": platform.machine(), "cpu": cpu,
            "cpus": os.cpu_count(), "memory_kib": ram, "kernel": platform.release(),
            "image_version": os.environ.get("ImageVersion")}


def collect(plan, suite, out, baseline_out, previous, output, rounds):
    if rounds < 3:
        raise ValueError("release benchmarks need at least three rounds")
    if plan["version"].startswith("dev-"):
        if not re.fullmatch(r"dev-[0-9a-f]{12}", plan["version"]):
            raise ValueError("invalid dry-run version")
    else:
        version_key(plan["version"])
    if bool(plan["baseline"]) != bool(baseline_out):
        raise ValueError("baseline build must match the plan")
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    result = dict(plan, schema_version=1, suite_id=suite["id"], rounds=rounds,
                  recorded_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  runner=runner_info(suite["runner"]), run_url=os.environ.get("BENCH_RUN_URL"),
                  workloads={}, metrics={}, errors=[], regressions=[])
    for workload in suite["workloads"]:
        name = workload["id"]
        result["workloads"][name] = {"command": workload["command"], "env": workload["env"], "runs": {}}
        reports = {}
        for phase in (("AA", "AB") if baseline_out else ("AA",)):
            print(f"{name}: {phase}, {rounds} rounds", flush=True)
            try:
                reports[phase], errors = measure(workload, phase,
                    baseline_out if phase == "AB" else out, out,
                    rounds, suite["timeout_seconds"], output)
                result["workloads"][name]["runs"][phase] = reports[phase]["runs"]
                result["errors"].extend(f"{name} {phase}: {error}" for error in errors)
                if errors:
                    break
            except ValueError as err:
                result["errors"].append(str(err))
                break
        current = reports.get("AB", reports.get("AA"))
        if not current or "AA" not in reports:
            continue
        runs = [run for run in current["runs"]
                if run["returncode"] == 0 and (not baseline_out or run["arm"] == "B")]
        names = sorted({metric for run in runs for metric in run["metrics"]})
        for metric in names:
            key = f"{name}/{metric}"
            direction = workload["headline"].get(metric)
            aa = paired(reports["AA"], metric)
            aa_noise = max(max(abs(x - 1), abs(1 / x - 1)) for x in aa["samples"]) if aa else None
            old_noise = ((previous or {}).get("metrics", {}).get(key, {}).get("aa_noise") or 0)
            noise = max(aa_noise or 0, old_noise) if direction else None
            ratio = paired(reports["AB"], metric) if "AB" in reports else None
            degradation = ((1 - ratio["median"]) if direction == "higher" else (ratio["median"] - 1)
                           ) if ratio and direction else None
            regressed = (degradation is not None and degradation > noise
                         and not math.isclose(degradation, noise, rel_tol=1e-12, abs_tol=1e-12))
            previous_runs = [run["metrics"][metric] for run in reports.get("AB", {}).get("runs", [])
                             if run["arm"] == "A" and run["returncode"] == 0 and metric in run["metrics"]]
            result["metrics"][key] = {
                "direction": direction, "current": summary([run["metrics"][metric] for run in runs
                                                            if metric in run["metrics"]]),
                "previous": summary(previous_runs), "paired_ratio": ratio, "aa_ratio": aa,
                "aa_noise": aa_noise, "noise_floor": noise, "degradation": degradation,
                "regression": regressed}
            if regressed:
                result["regressions"].append(key)
    result["complete"] = not result["errors"]
    result["status"] = ("failed" if not result["complete"] else
                        "comparison_unavailable" if plan["baseline_state"] in ("suite_changed", "runner_changed") else
                        "regression" if result["regressions"] else
                        "pass" if baseline_out else "first_measurement")
    (output / f"{plan['version']}.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    return result


def fmt_summary(stats):
    return f"{stats['median']:.4g} [{stats['min']:.4g}, {stats['max']:.4g}]" if stats else "—"


def release_table(record):
    runner = record["runner"]
    baseline = record["baseline"]
    lines = [f"### {record['version']}", "",
             f"Commit: `{record['commit']}`. Suite: `{record['suite_id']}`. "
             f"Runner: `{runner['type']}`, {runner['architecture']}, {runner['cpu']}, {runner['cpus']} CPUs.",
             f"{record['rounds']} paired rounds; **{record['status']}**. "
             f"Previous: `{baseline['version']}` (rebuilt on this runner)." if baseline else
             f"{record['rounds']} A/A rounds; **{record['status']}**. No comparable previous measurement.",
             ""]
    if record.get("run_url"):
        lines += [f"[Run and raw logs]({record['run_url']}) · [JSON](../bench/results/{record['version']}.json)", ""]
    if record["baseline_state"] in ("suite_changed", "runner_changed"):
        lines += ["**Comparison blocked:** the previous published suite or runner differs. "
                  "This is not a passing regression check.", ""]
    lines += ["Medians and spread are `[min, max]`. Positive degradation is worse; negative is better.",
              "", "| Headline metric | Current | Previous (same runner) | Paired B/A [min, max] | Degradation | Noise floor | Verdict |",
              "| --- | --- | --- | --- | --- | --- | --- |"]
    for name, metric in sorted(record["metrics"].items()):
        if not metric["direction"]:
            continue
        delta = f"{metric['degradation']:+.1%}" if metric["degradation"] is not None else "—"
        floor = f"{metric['noise_floor']:.1%}" if metric["noise_floor"] is not None else "—"
        verdict = "REGRESSION" if metric["regression"] else "within noise" if metric["paired_ratio"] else "baseline only"
        lines.append(f"| `{name}` | {fmt_summary(metric['current'])} | {fmt_summary(metric['previous'])} | "
                     f"{fmt_summary(metric['paired_ratio'])} | {delta} | {floor} | {verdict} |")
    lines += ["", "| A/A noise row (identical candidate build) | Paired B/A median [min, max] |",
              "| --- | --- |"]
    for name, metric in sorted(record["metrics"].items()):
        if metric["direction"]:
            lines.append(f"| `{name}` | {fmt_summary(metric['aa_ratio'])} |")
    if record["errors"]:
        lines += ["", "**Failed samples (not a publishable measurement):**", ""]
        lines += [f"- {error}" for error in record["errors"]]
    return "\n".join(lines)


def render(records, path):
    path = Path(path)
    text = path.read_text()
    if text.count(START) != 1 or text.count(END) != 1:
        raise ValueError("the benchmarks page must contain one results marker pair")
    before, rest = text.split(START)
    _, after = rest.split(END)
    tables = "\n\n".join(release_table(record) for record in reversed(records))
    path.write_text(before + START + "\n\n" + (tables or "No published measurements yet.") + "\n\n" + END + after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    plan_args = sub.add_parser("plan")
    plan_args.add_argument("--version", default="")
    plan_args.add_argument("--history", required=True)
    plan_args.add_argument("--output", required=True)
    plan_args.add_argument("--publish", action="store_true")
    run_args = sub.add_parser("collect")
    run_args.add_argument("--plan", required=True)
    run_args.add_argument("--out", required=True)
    run_args.add_argument("--baseline-out")
    run_args.add_argument("--history", required=True)
    run_args.add_argument("--output-dir", required=True)
    run_args.add_argument("--rounds", type=int)
    render_args = sub.add_parser("render")
    render_args.add_argument("--history", required=True)
    render_args.add_argument("--page", required=True)
    args = parser.parse_args()
    try:
        records = history(args.history)
        if args.mode == "render":
            render(records, args.page)
            return 0
        suite, fingerprint = load_suite()
        if args.mode == "plan":
            plan = make_plan(args.version, records, suite, fingerprint, args.publish)
            Path(args.output).write_text(json.dumps(plan, indent=2) + "\n")
            for key in ("version", "commit", "baseline_state"):
                print(f"{key}={plan[key]}")
            print(f"baseline_commit={(plan['baseline'] or {}).get('commit', '')}")
            return 0
        plan = json.loads(Path(args.plan).read_text())
        if plan["suite_fingerprint"] != fingerprint:
            raise ValueError("the suite changed after planning")
        previous = next((record for record in records
                         if record["version"] == (plan["baseline"] or {}).get("version")), None)
        result = collect(plan, suite, args.out, args.baseline_out, previous,
                         args.output_dir, args.rounds if args.rounds is not None else suite["rounds"])
        print(release_table(result))
        return 0 if result["status"] in ("pass", "first_measurement") else 1
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as err:
        print(f"release benchmarks: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
