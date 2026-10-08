import copy
import io
import json
import os
from pathlib import Path
import shutil
import tarfile
import unittest
from unittest.mock import patch
import uuid

import release_artifact as artifact


class ReleaseArtifactTest(unittest.TestCase):
    def setUp(self):
        self.base = Path.cwd() / ".ci-checks" / f"artifact-fixture-{uuid.uuid4().hex}"
        self.root = self.base / "orly"
        self.root.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.base)
        self.expected = {
            "schema": 1, "configuration": "release", "repository": "orlyatomics/orly",
            "sha": "a" * 40, "run_id": "1", "producer_attempt": "1",
            "os": "Linux", "arch": "X64", "compiler": {"version": "13", "target": "x86_64"},
            "source_root": str(self.root), "output_root": str(artifact.output_root(self.root)),
        }
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = (62).to_bytes(2, "little")
        for name in artifact.BINARIES:
            path = artifact.output_root(self.root) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(header)
            path.chmod(0o755)
        self.generated = artifact.output_root(self.root) / "orly/package.bison.hh"
        self.generated.write_text("#pragma once\n")
        self.generated.chmod(0o644)
        self.version = self.root / "orly/build_version.h"
        self.version.parent.mkdir(parents=True)
        self.version.write_text('#define ORLY_BUILD_VERSION "test"\n')
        self.version.chmod(0o644)
        self.archive = self.base / "release.tar.gz"
        self.digest = artifact.pack(self.root, self.archive, self.expected)

    def rewrite(self, transform):
        with tarfile.open(self.archive, "r:gz") as bundle:
            entries = [(member, bundle.extractfile(member).read()) for member in bundle.getmembers()]
        entries = transform(entries)
        with tarfile.open(self.archive, "w:gz") as bundle:
            for member, data in entries:
                member.size = len(data)
                bundle.addfile(member, io.BytesIO(data))
        self.digest = artifact.sha256(self.archive)

    def restore(self, expected=None):
        artifact.unpack(self.root, self.archive, expected or self.expected, self.digest)

    def test_complete_round_trip_and_permissions(self):
        shutil.rmtree(artifact.output_root(self.root))
        self.version.unlink()
        self.restore()
        for name in artifact.BINARIES:
            path = artifact.output_root(self.root) / name
            self.assertEqual(0o755, path.stat().st_mode & 0o777)
            self.assertEqual(b"\x7fELF", path.read_bytes()[:4])
        self.assertEqual("#pragma once\n", self.generated.read_text())
        self.assertIn("ORLY_BUILD_VERSION", self.version.read_text())
        self.assertEqual(0o644, self.generated.stat().st_mode & 0o777)

    def test_context_mismatch_is_rejected(self):
        for field, value in (
            ("sha", "b" * 40), ("run_id", "2"), ("producer_attempt", "2"),
            ("arch", "ARM64"), ("compiler", {"version": "14"}),
            ("source_root", "/different/source"), ("output_root", "/different/output"),
            ("configuration", "debug"), ("repository", "elsewhere/orly"),
        ):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.restore(dict(self.expected, **{field: value}))

    def test_partial_rerun_keeps_original_producer_attempt(self):
        context = {
            "GITHUB_SHA": "a" * 40, "GITHUB_REPOSITORY": "orlyatomics/orly",
            "GITHUB_RUN_ID": "1", "GITHUB_RUN_ATTEMPT": "2",
            "RUNNER_OS": "Linux", "RUNNER_ARCH": "X64",
        }
        with patch.dict(os.environ, context, clear=True), \
                patch.object(artifact.subprocess, "check_output", return_value="a" * 40), \
                patch.object(artifact, "compiler_identity", return_value=self.expected["compiler"]):
            actual = artifact.context(self.root, producer_attempt="1")
        self.assertEqual(self.expected, actual)
        self.restore(actual)

    def test_outer_checksum_is_required(self):
        with self.assertRaises(ValueError):
            artifact.unpack(self.root, self.archive, self.expected, "0" * 64)

    def test_payload_checksum_is_required(self):
        def change(entries):
            for index, (member, data) in enumerate(entries):
                if member.name == "output/orly/orlyc":
                    entries[index] = (member, data[:32] + bytes([data[32] ^ 0xff]) + data[33:])
            return entries
        self.rewrite(change)
        with self.assertRaises(ValueError):
            self.restore()

    def test_duplicate_entry_is_rejected(self):
        self.rewrite(lambda entries: entries + [copy.copy(entries[1])])
        with self.assertRaises(ValueError):
            self.restore()

    def test_non_regular_entry_is_rejected(self):
        def change(entries):
            entries[1][0].type = tarfile.SYMTYPE
            entries[1][0].linkname = "other"
            return entries
        self.rewrite(change)
        with self.assertRaises(ValueError):
            self.restore()

    def test_paths_cannot_escape(self):
        for name in (
            "output/../../escape", "/absolute.h", "source/../escape.h",
            "output/a\\b.h", "source/tracked.h", "output/file.o", "output//header.h",
        ):
            self.assertFalse(artifact.allowed_name(name), name)
            with self.assertRaises(ValueError):
                artifact.destination(self.root, name)

    def test_manifest_traversal_is_rejected_before_any_write(self):
        name = "output/../../escape"
        def change(entries):
            manifest = json.loads(entries[0][1])
            manifest["files"][name] = {"size": 1, "mode": 0o644, "sha256": "0" * 64}
            entries[0] = (entries[0][0], json.dumps(manifest).encode())
            member = tarfile.TarInfo(name)
            member.mode = 0o644
            return entries + [(member, b"x")]
        self.rewrite(change)
        before = self.version.read_bytes()
        with self.assertRaises(ValueError):
            self.restore()
        self.assertEqual(before, self.version.read_bytes())
        self.assertFalse((self.base / "escape").exists())

    def test_missing_binary_is_rejected(self):
        name = "output/orly/orlyc"
        def change(entries):
            manifest = json.loads(entries[0][1])
            manifest["files"].pop(name)
            entries[0] = (entries[0][0], json.dumps(manifest).encode())
            return [(member, data) for member, data in entries if member.name != name]
        self.rewrite(change)
        with self.assertRaises(ValueError):
            self.restore()

    def test_wrong_elf_architecture_is_rejected(self):
        with self.assertRaises(ValueError):
            artifact.pack(self.root, self.archive, dict(self.expected, arch="ARM64"))

    def test_symlink_payload_is_rejected(self):
        self.generated.unlink()
        self.generated.symlink_to(self.version)
        with self.assertRaises(ValueError):
            artifact.pack(self.root, self.archive, self.expected)

    def test_symlink_destination_is_rejected(self):
        self.generated.unlink()
        self.generated.symlink_to(self.version)
        with self.assertRaises(ValueError):
            self.restore()


if __name__ == "__main__":
    unittest.main()
