#!/usr/bin/env python3
"""Package release smoke inputs and verify them before restoring a build."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile


UNIT_TARGETS = (
    "orly/atom/kit2", "orly/indy/key", "orly/indy/prefix_match", "orly/indy/repo",
    "orly/indy/disk/durable_manager", "orly/indy/disk/merge_data_file", "orly/indy/disk/fold_data_file",
    "orly/indy/context_fold", "orly/indy/fault_injection",
)
BINARIES = (
    "orly/orlyc", "orly/server/orlyi", "orly/client/orly_client",
    "orly/indy/disk/util/orly_dm",
) + tuple(f"{target}.test" for target in UNIT_TARGETS)
HEADER_SUFFIXES = {".h", ".hh", ".hpp"}
MACHINES = {"X64": 62, "ARM64": 183}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def output_root(root):
    return root.parent / "out_orly" / "release"


def compiler_identity():
    compiler = "/usr/bin/g++"
    query = lambda flag: subprocess.check_output([compiler, flag], text=True).strip()
    paths = [Path(compiler), Path("/usr/bin/gcc")]
    for name in ("cc1plus", "cc1", "collect2", "lto1", "lto-wrapper"):
        paths.append(Path(query(f"-print-prog-name={name}")))
    paths.append(Path(query("-print-file-name=liblto_plugin.so")))
    return {
        "version": query("-dumpfullversion"),
        "target": query("-dumpmachine"),
        "inputs": {str(path.resolve()): sha256(path) for path in paths},
    }


def context(root, producer_attempt=None):
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    if commit != os.environ["GITHUB_SHA"]:
        raise ValueError("checkout is not the tested workflow SHA")
    if os.environ["RUNNER_OS"] != "Linux" or os.environ["RUNNER_ARCH"] not in MACHINES:
        raise ValueError("unsupported release platform")
    return {
        "schema": 1, "configuration": "release",
        "repository": os.environ["GITHUB_REPOSITORY"], "sha": commit,
        "run_id": os.environ["GITHUB_RUN_ID"],
        "producer_attempt": producer_attempt or os.environ["GITHUB_RUN_ATTEMPT"],
        "os": os.environ["RUNNER_OS"], "arch": os.environ["RUNNER_ARCH"],
        "compiler": compiler_identity(), "source_root": str(root),
        "output_root": str(output_root(root)),
    }


def allowed_name(name):
    path = PurePosixPath(name)
    if not name or "\\" in name or path.is_absolute() or any(part in ("", ".", "..") for part in name.split("/")):
        return False
    relative = PurePosixPath(*path.parts[1:])
    if path.parts[0] == "output":
        return str(relative) in BINARIES or relative.suffix in HEADER_SUFFIXES
    if path.parts[0] == "source":
        return str(relative) == "orly/build_version.h" or (
            relative.parts[:3] == ("orly", "rt", "objects") and relative.suffix in HEADER_SUFFIXES
        )
    return False


def check_elf(header, arch, mode):
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[4:6] != b"\x02\x01":
        raise ValueError("release binary is not little-endian ELF64")
    if int.from_bytes(header[18:20], "little") != MACHINES[arch] or not mode & 0o111:
        raise ValueError("release binary architecture/permissions mismatch")


def payload_files(root):
    output = output_root(root)
    files = {f"output/{name}": output / name for name in BINARIES}
    for path in output.rglob("*"):
        if path.suffix in HEADER_SUFFIXES and path.is_file():
            files[f"output/{path.relative_to(output).as_posix()}"] = path
    version = root / "orly/build_version.h"
    if version.exists():
        files["source/orly/build_version.h"] = version
    objects = root / "orly/rt/objects"
    for path in objects.rglob("*"):
        if path.suffix in HEADER_SUFFIXES and path.is_file():
            files[f"source/{path.relative_to(root).as_posix()}"] = path
    return files


def pack(root, archive, expected):
    files = payload_files(root)
    records = {}
    for name, path in sorted(files.items()):
        base = output_root(root) if name.startswith("output/") else root
        if not allowed_name(name) or path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(base.resolve()):
            raise ValueError(f"invalid release payload: {name}")
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & ~0o777:
            raise ValueError("release payload has special permissions")
        if name.removeprefix("output/") in BINARIES:
            with path.open("rb") as source:
                check_elf(source.read(20), expected["arch"], mode)
        records[name] = {"sha256": sha256(path), "size": path.stat().st_size, "mode": mode}
    manifest = json.dumps({"context": expected, "files": records}, sort_keys=True).encode()
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "w:gz", compresslevel=1) as bundle:
        info = tarfile.TarInfo("manifest.json")
        info.size, info.mode = len(manifest), 0o644
        bundle.addfile(info, io.BytesIO(manifest))
        for name, path in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = records[name]["size"], records[name]["mode"]
            with path.open("rb") as source:
                bundle.addfile(info, source)
    return sha256(archive)


def destination(root, name):
    if not allowed_name(name):
        raise ValueError(f"unexpected release payload path: {name}")
    prefix, relative = name.split("/", 1)
    base = output_root(root) if prefix == "output" else root
    path = base / relative
    if path.is_symlink() or not path.resolve().is_relative_to(base.resolve()):
        raise ValueError("release payload destination escapes its root")
    return path


def unpack(root, archive, expected, digest):
    if not re.fullmatch(r"[0-9a-f]{64}", digest) or sha256(archive) != digest:
        raise ValueError("release archive checksum mismatch")
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        names = [member.name for member in members]
        if len(names) != len(set(names)) or any(not member.isfile() for member in members):
            raise ValueError("release archive contains duplicate or non-regular entries")
        manifest_member = bundle.getmember("manifest.json")
        if manifest_member.size > 1024 * 1024:
            raise ValueError("release manifest is too large")
        manifest = json.load(bundle.extractfile(manifest_member))
        if manifest["context"] != expected:
            raise ValueError("release archive run/SHA/platform/compiler/layout mismatch")
        records = manifest["files"]
        if set(names) != {"manifest.json", *records} or not {f"output/{name}" for name in BINARIES} <= set(records):
            raise ValueError("release archive payload list is incomplete")
        # Verify everything before writing; no extraction follows tar links.
        for member in members:
            if member.name == "manifest.json":
                continue
            destination(root, member.name)
            record = records[member.name]
            if member.mode != record["mode"] or member.size != record["size"] or member.mode & ~0o777:
                raise ValueError("release archive permissions/size mismatch")
            content = bundle.extractfile(member)
            header = content.read(20)
            digest = hashlib.sha256(header)
            for block in iter(lambda: content.read(1024 * 1024), b""):
                digest.update(block)
            if digest.hexdigest() != record["sha256"]:
                raise ValueError("release payload checksum mismatch")
            if member.name.removeprefix("output/") in BINARIES:
                check_elf(header, expected["arch"], member.mode)
        for member in members:
            if member.name == "manifest.json":
                continue
            path = destination(root, member.name)
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("wb") as output, bundle.extractfile(member) as source:
                shutil.copyfileobj(source, output)
            path.chmod(member.mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("pack", "unpack"))
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--sha256")
    parser.add_argument("--producer-attempt")
    args = parser.parse_args()
    root = args.root.resolve()
    expected = context(root, args.producer_attempt)
    if args.operation == "pack":
        digest = pack(root, args.archive, expected)
        if "GITHUB_OUTPUT" in os.environ:
            with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
                output.write(f"sha256={digest}\nproducer-attempt={expected['producer_attempt']}\n")
        print(f"Release archive: {args.archive.stat().st_size} bytes, sha256={digest}")
    else:
        if not args.sha256 or not args.producer_attempt:
            parser.error("unpack requires the producer checksum and attempt")
        unpack(root, args.archive, expected, args.sha256)
        print("Release archive verified and restored")


if __name__ == "__main__":
    main()
