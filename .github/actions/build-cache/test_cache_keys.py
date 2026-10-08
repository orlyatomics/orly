from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shutil
import unittest
from unittest.mock import patch
import uuid

import cache_keys


class CacheKeysTest(unittest.TestCase):
    def setUp(self):
        self.context = {
            "RUNNER_OS": "Linux", "RUNNER_ARCH": "X64",
            "GITHUB_REPOSITORY": "orlyatomics/orly",
            "GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/master",
            "GITHUB_JOB": "debug-and-test", "GITHUB_SHA": "a" * 40,
            "GITHUB_RUN_ID": "1", "GITHUB_RUN_ATTEMPT": "1",
        }
        self.root = Path.cwd() / ".ci-checks" / f"cache-fixture-{uuid.uuid4().hex}"
        self.root.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.root)

    def test_writer_policy(self):
        for (arch, config), job in cache_keys.WRITERS.items():
            context = dict(self.context, RUNNER_ARCH=arch, GITHUB_JOB=job)
            self.assertTrue(cache_keys.trusted_writer(context, config))
            for field, value in (
                ("GITHUB_EVENT_NAME", "pull_request"),
                ("GITHUB_EVENT_NAME", "schedule"),
                ("GITHUB_EVENT_NAME", "workflow_dispatch"),
                ("GITHUB_REF", "refs/tags/v1"),
                ("GITHUB_REF", "refs/heads/release"),
                ("GITHUB_REPOSITORY", "elsewhere/orly"),
                ("GITHUB_JOB", "examples"),
            ):
                self.assertFalse(cache_keys.trusted_writer(dict(context, **{field: value}), config))

    def test_namespace_isolation(self):
        baseline = cache_keys.namespace("Linux", "X64", "gcc13-abc", "debug")
        for args in (
            ("Linux", "ARM64", "gcc13-abc", "debug"),
            ("Linux", "X64", "gcc14-abc", "debug"),
            ("Linux", "X64", "gcc13-xyz", "debug"),
            ("Linux", "X64", "gcc13-abc", "release"),
        ):
            self.assertNotEqual(baseline, cache_keys.namespace(*args))
        with self.assertRaises(ValueError):
            cache_keys.namespace("Linux\n", "X64", "gcc13", "debug")
        self.assertFalse(cache_keys.trusted_writer(dict(self.context, RUNNER_ARCH="other", GITHUB_JOB=None), "debug"))

    def test_immutable_snapshot_and_seed_writers(self):
        first = cache_keys.settings(self.context, "debug", "gcc13-abc", "seed", True)
        context = dict(self.context, GITHUB_RUN_ATTEMPT="2")
        self.assertNotEqual(first["ORLY_CCACHE_KEY"],
                            cache_keys.settings(context, "debug", "gcc13-abc", "seed", True)["ORLY_CCACHE_KEY"])
        self.assertEqual("true", first["ORLY_BOOTSTRAP_WRITE"])
        context = dict(self.context, GITHUB_JOB="release-build")
        self.assertEqual("false", cache_keys.settings(context, "release", "gcc13-abc", "seed", True)["ORLY_BOOTSTRAP_WRITE"])
        self.assertEqual("false", cache_keys.settings(self.context, "debug", "gcc13-abc", "seed", False)["ORLY_CCACHE_WRITE"])
        self.assertEqual("1", cache_keys.settings(self.context, "debug", "gcc13-abc", "seed", False)["CCACHE_DISABLE"])
        self.assertEqual("false", cache_keys.settings(self.context, "debug", "gcc13-abc", "", True)["ORLY_BOOTSTRAP_WRITE"])

    def test_digest_covers_sources_dependencies_and_layout(self):
        source = self.root / "seed.cc"
        header = self.root / "seed.h"
        source.write_text('#include "seed.h"\n')
        header.write_text("old\n")
        first = cache_keys.files_digest((source, header), (str(self.root), "gcc13"))
        header.write_text("new\n")
        self.assertNotEqual(first, cache_keys.files_digest((source, header), (str(self.root), "gcc13")))
        header.write_text("old\n")
        self.assertNotEqual(first, cache_keys.files_digest((source, header), ("/another/root", "gcc13")))
        self.assertNotEqual(first, cache_keys.files_digest((source, header), (str(self.root), "gcc14")))
        self.assertEqual(first, cache_keys.files_digest((header, source), (str(self.root), "gcc13")))

    def write_bootstrap(self, extra=""):
        (self.root / "bootstrap.sh").write_text(
            'CC=g++\ncommon_flags=(\n  -O3 -std=c++23\n  )\n'
            '$CC -o tools/jhm "${common_flags[@]}" base/first.cc \\\n'
            f'  -I./ -DSRC_ROOT=\\"`pwd`\\" -pthread {extra}\n'
            '$CC -o tools/make_dep_file "${common_flags[@]}" jhm/second.cc \\\n'
            '  -I./ -DSRC_ROOT=\\"`pwd`\\" -pthread\n'
        )

    def test_bootstrap_commands_preserve_flags_and_sources(self):
        self.write_bootstrap()
        commands = cache_keys.bootstrap_commands(self.root)
        self.assertEqual(2, len(commands))
        self.assertIn("-O3", commands[0])
        self.assertIn("-std=c++23", commands[0])
        self.assertIn("base/first.cc", commands[0])
        self.assertIn("jhm/second.cc", commands[1])
        self.assertIn('-DSRC_ROOT="' + str(self.root) + '"', commands[0])
        self.assertEqual(["-M", "-MT", "seed-cache"], commands[0][-3:])
        self.assertNotIn("-o", commands[0])
        self.assertNotIn("tools/jhm", commands[0])

    def test_unsupported_bootstrap_fails_closed_to_no_cache(self):
        for extra in ("$EXTRA_FLAGS", "`another-command`", "*.cc", "-MF unexpected", "-luuid", "-march=native", "-static"):
            self.write_bootstrap(extra)
            with self.assertRaises(ValueError):
                cache_keys.bootstrap_commands(self.root)
        self.write_bootstrap()
        text = (self.root / "bootstrap.sh").read_text().replace("tools/jhm", "tools/other")
        (self.root / "bootstrap.sh").write_text(text)
        with self.assertRaises(ValueError):
            cache_keys.bootstrap_commands(self.root)
        self.write_bootstrap()
        text = (self.root / "bootstrap.sh").read_text().replace("-O3", "$EXTRA_FLAGS")
        (self.root / "bootstrap.sh").write_text(text)
        with self.assertRaises(ValueError):
            cache_keys.bootstrap_commands(self.root)

    def test_failed_dependency_scan_is_a_cold_build(self):
        output = self.root / "environment"
        context = dict(self.context, GITHUB_WORKSPACE=str(self.root), GITHUB_ENV=str(output))
        with patch.dict(os.environ, context, clear=True), \
                patch("sys.argv", ["cache_keys.py", "--config", "debug"]), \
                patch.object(cache_keys, "compiler_identity", return_value="gcc13-abc"), \
                patch.object(cache_keys, "bootstrap_key", side_effect=ValueError("unknown dependencies")), \
                redirect_stdout(io.StringIO()):
            cache_keys.main()
        self.assertIn("ORLY_BOOTSTRAP_KEY=\n", output.read_text())
        self.assertIn("ORLY_BOOTSTRAP_WRITE=false\n", output.read_text())
        self.assertIn("ORLY_CCACHE_WRITE=true\n", output.read_text())

    def test_dependency_closure_includes_system_headers(self):
        paths = cache_keys.dependency_paths(
            "seed-cache: base/first.cc base/seed.h \\\n /usr/include/stdio.h\n"
            "seed-cache: jhm/second.cc base/seed.h\n", self.root)
        self.assertEqual({
            self.root / "base/first.cc", self.root / "base/seed.h",
            self.root / "jhm/second.cc", Path("/usr/include/stdio.h"),
        }, paths)
        with self.assertRaises(ValueError):
            cache_keys.dependency_paths("unexpected: seed.h", self.root)


if __name__ == "__main__":
    unittest.main()
