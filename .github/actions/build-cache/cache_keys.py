#!/usr/bin/env python3
"""Compute isolated compiler snapshots and exact bootstrap dependency keys."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


WRITERS = {
    ("X64", "debug"): "debug-and-test",
    ("X64", "release"): "release-build",
    ("X64", "asan"): "asan-smoke",
    ("X64", "tsan"): "tsan",
    ("ARM64", "debug"): "arm64-build",
    ("ARM64", "release"): "arm64-release",
}
SEED_WRITERS = {"X64": "debug-and-test", "ARM64": "arm64-build"}
COMPILER = "/usr/bin/g++"


def command(args, root=None):
    return subprocess.check_output(args, cwd=root, text=True, timeout=120).strip()


def files_digest(paths, extra=()):
    digest = hashlib.sha256(json.dumps(extra, sort_keys=True).encode())
    for path in sorted({Path(p).resolve() for p in paths}):
        digest.update(str(path).encode() + b"\0")
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        digest.update(b"\0")
    return digest.hexdigest()


def compiler_identity():
    version = command([COMPILER, "-dumpfullversion", "-dumpversion"])
    target = command([COMPILER, "-dumpmachine"])
    paths = [COMPILER, "/usr/bin/gcc"]
    for name in ("cc1plus", "cc1", "collect2", "lto1", "lto-wrapper"):
        paths.append(command([COMPILER, f"-print-prog-name={name}"]))
    paths.append(command([COMPILER, "-print-file-name=liblto_plugin.so"]))
    fingerprint = files_digest(paths, (version, target))
    return f"gcc{version}-{fingerprint[:16]}"


def namespace(os_name, arch, compiler, config):
    parts = ("orly-ccache-v2", os_name, arch, compiler, config)
    if not all(re.fullmatch(r"[A-Za-z0-9._-]+", part) for part in parts):
        raise ValueError("invalid compiler cache namespace")
    return "-".join(parts)


def trusted_writer(context, config):
    expected_job = WRITERS.get((context.get("RUNNER_ARCH"), config))
    return (
        expected_job is not None
        and context.get("GITHUB_REPOSITORY") == "orlyatomics/orly"
        and context.get("GITHUB_EVENT_NAME") == "push"
        and context.get("GITHUB_REF") == "refs/heads/master"
        and context.get("GITHUB_JOB") == expected_job
    )


def bootstrap_commands(root):
    text = (root / "bootstrap.sh").read_text()
    if not re.search(r"(?m)^CC=g\+\+$", text):
        raise ValueError("unsupported bootstrap compiler")
    match = re.search(r"common_flags=\((.*?)\n\s*\)", text, re.S)
    if not match:
        raise ValueError("cannot read bootstrap flags")
    flags = shlex.split(match.group(1), comments=True)
    commands = []
    targets = []
    for line in text.replace("\\\n", " ").splitlines():
        if not line.startswith("$CC "):
            continue
        args = shlex.split(line)
        if args[1] != "-o":
            raise ValueError("unsupported bootstrap output")
        targets.append(args[2])
        argv = [COMPILER]
        for arg in args[3:]:
            if arg == "${common_flags[@]}":
                argv.extend(flags)
            elif arg == '-DSRC_ROOT="`pwd`"':
                argv.append("-DSRC_ROOT=" + json.dumps(str(root)))
            elif any(char in arg for char in ("$", "`", "*", "?")):
                raise ValueError("unsupported dynamic bootstrap argument")
            else:
                argv.append(arg)
        # Dependency-only mode must not inherit another output file option.
        if any(arg in ("-o", "-MF", "-MT", "-MQ") or arg.startswith("-save-temps") for arg in argv):
            raise ValueError("unsupported bootstrap dependency output")
        if any(any(char in arg for char in ("$", "`", "*", "?")) for arg in argv):
            raise ValueError("unsupported dynamic bootstrap flags")
        if any(arg.startswith("-l") or ",-l" in arg for arg in argv):
            raise ValueError("bootstrap link dependencies changed")
        if any(arg.startswith("-static") for arg in argv):
            raise ValueError("bootstrap static link dependencies changed")
        if any(arg in ("-march=native", "-mcpu=native", "-mtune=native") for arg in argv):
            raise ValueError("bootstrap requires a machine-specific key")
        commands.append(argv + ["-M", "-MT", "seed-cache"])
    if targets != ["tools/jhm", "tools/make_dep_file"]:
        raise ValueError("bootstrap seed targets changed")
    return commands


def dependency_paths(output, root):
    paths = set()
    for line in output.replace("\\\n", " ").splitlines():
        target, separator, dependencies = line.partition(":")
        if not separator or target != "seed-cache":
            raise ValueError("cannot parse bootstrap dependencies")
        for name in shlex.split(dependencies):
            paths.add((root / name).resolve())
    return paths


def bootstrap_key(root, compiler, os_name, arch):
    inputs = {root / "bootstrap.sh", Path(__file__)}
    for argv in bootstrap_commands(root):
        inputs.update(dependency_paths(command(argv, root), root))
    for path in inputs:
        if path.is_relative_to(root) and path.suffix in (".h", ".cc", ".c", ".inc"):
            if re.search(rb"\b__(?:DATE|TIME|TIMESTAMP)__\b", path.read_bytes()):
                raise ValueError("bootstrap contains a volatile compilation timestamp")
    # Include default linker/runtime and startup inputs, not just headers.
    inputs.add(Path("/usr/bin/ld"))
    for name in (
        "libstdc++.so", "libstdc++.a", "libgcc_s.so.1", "libgcc.a",
        "libc.so.6", "libc.a", "libm.so.6", "libm.a",
        "libgcc_eh.a", "crt1.o", "Scrt1.o", "crti.o", "crtn.o",
        "crtbegin.o", "crtbeginS.o", "crtend.o", "crtendS.o",
    ):
        inputs.add(Path(command([COMPILER, f"-print-file-name={name}"])))
    digest = files_digest(inputs, (str(root), compiler, os_name, arch))
    return f"orly-bootstrap-v1-{os_name}-{arch}-{digest}"


def settings(context, config, compiler, seed_key, enabled):
    prefix = namespace(context["RUNNER_OS"], context["RUNNER_ARCH"], compiler, config)
    suffix = "-".join(context.get(name, "local") for name in (
        "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT",
    ))
    can_write = enabled and trusted_writer(context, config)
    seed_writer = can_write and context["GITHUB_JOB"] == SEED_WRITERS.get(context["RUNNER_ARCH"])
    values = {
        "ORLY_CCACHE_PREFIX": prefix,
        "ORLY_CCACHE_KEY": f"{prefix}-{suffix}",
        "ORLY_CCACHE_WRITE": str(can_write).lower(),
        "ORLY_BOOTSTRAP_KEY": seed_key,
        "ORLY_BOOTSTRAP_WRITE": str(bool(seed_key) and seed_writer).lower(),
        "CCACHE_COMPILERCHECK": "content",
    }
    if not enabled:
        values["CCACHE_DISABLE"] = "1"
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, choices=("debug", "release", "asan", "tsan"))
    parser.add_argument("--enabled", choices=("true", "false"), default="true")
    parser.add_argument("--bootstrap", choices=("true", "false"), default="true")
    args = parser.parse_args()
    context = os.environ
    compiler = compiler_identity()
    seed = ""
    if args.enabled == "true" and args.bootstrap == "true":
        start = time.monotonic()
        try:
            seed = bootstrap_key(Path(context["GITHUB_WORKSPACE"]).resolve(), compiler,
                                 context["RUNNER_OS"], context["RUNNER_ARCH"])
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            # A changed bootstrap form or missing dependency must produce a
            # cold build, never a guessed key or an untested successful job.
            print(f"::warning::Bootstrap cache disabled: {error}")
        print(f"Bootstrap dependency fingerprint: {time.monotonic() - start:.2f}s")
    values = settings(context, args.config, compiler, seed, args.enabled == "true")
    if "GITHUB_ENV" in context:
        with Path(context["GITHUB_ENV"]).open("a") as output:
            for name, value in values.items():
                if "\n" in value or "\r" in value:
                    raise ValueError("cache setting contains a newline")
                output.write(f"{name}={value}\n")
    print(json.dumps(values, indent=2))


if __name__ == "__main__":
    main()
