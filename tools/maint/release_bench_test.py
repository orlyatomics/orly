import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

import ab_bench
import release_bench as bench


def report(a, b):
    return {"runs": [{"round": r, "arm": arm, "returncode": 0, "log": f"{r}-{arm}.log",
                      "metrics": {"rate": values[r - 1]}}
                     for r in range(1, 4) for arm, values in (("A", a), ("B", b))]}


class ReleaseBenchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = bench.ROOT / "bench" / f".checks-{os.getpid()}"
        cls.work.mkdir()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def setUp(self):
        self.directory = self.work / self._testMethodName
        self.directory.mkdir()
        self.plan = {"version": "v1.1.0", "commit": "b" * 40, "baseline_state": "compatible",
                     "baseline": {"version": "v1.0.0", "commit": "a" * 40},
                     "suite_fingerprint": "fixture", "previous_published_version": "v1.0.0"}
        self.suite = {"id": "fixture", "runner": "ubuntu-24.04", "rounds": 3,
                      "timeout_seconds": 5,
                      "workloads": [{"id": "fixture", "command": [], "env": {}, "inputs": [],
                                     "headline": {"rate": "higher", "latency": "lower"}}]}

    def collect_fixture(self, code, baseline=True, previous=None):
        self.suite["workloads"][0]["command"] = [sys.executable, "-c", code]
        if not baseline:
            self.plan.update(baseline=None, baseline_state="first_measurement")
        with contextlib.redirect_stdout(io.StringIO()):
            return bench.collect(self.plan, self.suite, "candidate",
                                 "baseline" if baseline else None, previous, self.directory, 3)

    def test_release_version_order(self):
        versions = ["v1.0.0-rc.2", "v1.0.0", "v1.10.0", "v1.2.0", "v1.0.0-rc.10", "v1.0.0-1"]
        self.assertEqual(sorted(versions, key=bench.version_key),
                         ["v1.0.0-1", "v1.0.0-rc.2", "v1.0.0-rc.10", "v1.0.0", "v1.2.0", "v1.10.0"])
        for value in ("v01.0.0", "v1.0", "../v1.0.0", "v1.0.0-01", "v1.0.0-rc..1", "v1.0.0; echo x"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bench.version_key(value)

    def test_default_suite(self):
        suite, digest = bench.load_suite()
        self.assertEqual(len(digest), 64)
        self.assertEqual(suite["runner"], "ubuntu-24.04")
        self.assertEqual({w["id"] for w in suite["workloads"]},
                         {"pool-pressure", "batch-backlog", "read-fold", "ws-scale", "grc20", "restart"})
        self.assertEqual(sum(len(w["headline"]) for w in suite["workloads"]), 34)

    @patch("release_bench.subprocess.check_output", return_value="b" * 40 + "\n")
    def test_plan_uses_previous_release_not_future_or_same_version(self, git):
        records = [{"version": version, "commit": "a" * 40, "suite_fingerprint": "fixture",
                    "runner": {"type": "ubuntu-24.04"}}
                   for version in ("v1.0.0", "v1.1.0", "v1.2.0")]
        plan = bench.make_plan("v1.1.0", records, self.suite, "fixture", publish=True)
        self.assertEqual(plan["baseline"]["version"], "v1.0.0")
        self.assertEqual(plan["commit"], "b" * 40)
        self.assertEqual(git.call_args_list[0].args[0][-1], "refs/tags/v1.1.0^{commit}")

    @patch("release_bench.subprocess.check_output", return_value="b" * 40 + "\n")
    def test_dispatch_without_tag_is_a_dry_run(self, git):
        plan = bench.make_plan("", [], self.suite, "fixture")
        self.assertEqual(plan["version"], "dev-" + "b" * 12)
        self.assertIsNone(plan["baseline"])
        with self.assertRaises(ValueError):
            bench.make_plan("", [], self.suite, "fixture", publish=True)

    @patch("release_bench.subprocess.check_output", return_value="b" * 40 + "\n")
    def test_suite_or_runner_change_blocks_comparison(self, git):
        previous = {"version": "v1.0.0", "commit": "a" * 40,
                    "suite_fingerprint": "old", "runner": {"type": "ubuntu-24.04"}}
        plan = bench.make_plan("v1.1.0", [previous], self.suite, "fixture")
        self.assertEqual(plan["baseline_state"], "suite_changed")
        self.assertIsNone(plan["baseline"])
        previous.update(suite_fingerprint="fixture", runner={"type": "different-runner"})
        self.assertEqual(bench.make_plan("v1.1.0", [previous], self.suite, "fixture")["baseline_state"],
                         "runner_changed")

    def test_collect_improvement_and_all_raw_samples(self):
        result = self.collect_fixture(
            "import os; old=os.environ['ORLY_OUT'].endswith('baseline'); "
            "print('METRIC rate', 100 if old else 110); "
            "print('METRIC latency', 10 if old else 9); print('METRIC pool', 0)")
        self.assertTrue(result["complete"])
        self.assertEqual(result["status"], "pass")
        self.assertAlmostEqual(result["metrics"]["fixture/rate"]["paired_ratio"]["median"], 1.1)
        self.assertAlmostEqual(result["metrics"]["fixture/latency"]["degradation"], -0.1)
        self.assertEqual(result["metrics"]["fixture/rate"]["current"]["n"], 3)
        self.assertIsNone(result["metrics"]["fixture/pool"]["direction"])
        self.assertEqual(len(result["workloads"]["fixture"]["runs"]["AA"]), 6)
        self.assertEqual(len(result["workloads"]["fixture"]["runs"]["AB"]), 6)
        self.assertTrue(json.loads((self.directory / "v1.1.0.json").read_text())["complete"])

    def test_both_headline_directions_regress_beyond_noise(self):
        previous = {"metrics": {"fixture/rate": {"aa_noise": 0.02}}}
        result = self.collect_fixture(
            "import os; old=os.environ['ORLY_OUT'].endswith('baseline'); "
            "print('METRIC rate', 100 if old else 90); print('METRIC latency', 10 if old else 13)",
            previous=previous)
        self.assertTrue(result["complete"])
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["regressions"], ["fixture/latency", "fixture/rate"])
        self.assertEqual(result["metrics"]["fixture/rate"]["noise_floor"], 0.02)

    def test_small_change_within_observed_aa_noise_passes(self):
        self.suite["workloads"][0]["headline"] = {"rate": "higher"}
        aa = report([100, 100, 105], [105, 96, 100])
        ab = report([100, 100, 100], [96, 96, 96])
        with patch("release_bench.measure", side_effect=[(aa, []), (ab, [])]), \
                contextlib.redirect_stdout(io.StringIO()):
            result = bench.collect(self.plan, self.suite, "candidate", "baseline", None, self.directory, 3)
        self.assertEqual(result["status"], "pass")
        self.assertAlmostEqual(result["metrics"]["fixture/rate"]["degradation"], 0.04)
        self.assertGreater(result["metrics"]["fixture/rate"]["noise_floor"], 0.05)

    def test_change_at_the_noise_floor_is_not_a_regression(self):
        self.suite["workloads"][0]["headline"] = {"rate": "higher"}
        aa = report([100] * 3, [100] * 3)
        ab = report([100] * 3, [95] * 3)
        previous = {"metrics": {"fixture/rate": {"aa_noise": 0.05}}}
        with patch("release_bench.measure", side_effect=[(aa, []), (ab, [])]), \
                contextlib.redirect_stdout(io.StringIO()):
            result = bench.collect(self.plan, self.suite, "candidate", "baseline",
                                   previous, self.directory, 3)
        self.assertEqual(result["status"], "pass")

    def test_first_measurement_only_calibrates(self):
        result = self.collect_fixture("print('METRIC rate 100'); print('METRIC latency 10')", baseline=False)
        self.assertEqual(result["status"], "first_measurement")
        self.assertIsNone(result["metrics"]["fixture/rate"]["paired_ratio"])
        self.assertEqual(result["metrics"]["fixture/rate"]["current"]["n"], 6)

    def test_missing_headline_fails_despite_zero_exit(self):
        result = self.collect_fixture("print('METRIC pool 0')", baseline=False)
        self.assertFalse(result["complete"])
        self.assertEqual(result["status"], "failed")
        self.assertTrue(any("missing or nonpositive" in error for error in result["errors"]))

    def test_failed_runs_and_their_metrics_are_retained(self):
        result = self.collect_fixture(
            "import sys; print('METRIC rate 100'); print('METRIC latency 10'); sys.exit(7)", baseline=False)
        self.assertFalse(result["complete"])
        runs = result["workloads"]["fixture"]["runs"]["AA"]
        self.assertEqual(len(runs), 1)
        self.assertTrue(all(run["returncode"] == 7 and run["metrics"]["rate"] == 100 for run in runs))

    def test_invalid_metrics_and_missing_rounds_fail(self):
        for value in (0, -1, float("nan"), float("inf")):
            bad = report([100] * 3, [100] * 3)
            bad["runs"][0]["metrics"]["rate"] = value
            with self.subTest(value=value):
                self.assertTrue(bench.check_runs(bad, {"rate": "higher"}, 3))
        bad = report([100] * 3, [100] * 3)
        bad["runs"].pop()
        self.assertIn("missing or duplicate arm/round", bench.check_runs(bad, {"rate": "higher"}, 3))

    def test_timeout_is_failure(self):
        log = self.directory / "timeout.log"
        code, _ = ab_bench.run([sys.executable, "-c", "import time; time.sleep(60)"],
                               "candidate", log, timeout=0.05)
        self.assertEqual(code, 124)
        self.assertIn("TIMED OUT", log.read_text())

    def test_ab_json_preserves_alternating_order(self):
        data = self.directory / "runs.json"
        command = [sys.executable, str(bench.ROOT / "tools/maint/ab_bench.py"),
                   "--a", "candidate", "--b", "baseline", "--rounds", "2",
                   "--log-dir", str(self.directory / "logs"), "--json", str(data), "--",
                   sys.executable, "-c", "print('METRIC rate 100')"]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        runs = json.loads(data.read_text())["runs"]
        self.assertEqual([(run["round"], run["arm"]) for run in runs],
                         [(1, "A"), (1, "B"), (2, "B"), (2, "A")])

    def test_render_preserves_documentation_and_is_idempotent(self):
        result = self.collect_fixture("print('METRIC rate 100'); print('METRIC latency 10')", baseline=False)
        page = self.directory / "benchmarks.md"
        page.write_text(f"# Notes\n\n{bench.START}\nold tables\n{bench.END}\nFooter\n")
        bench.render([result], page)
        first = page.read_text()
        bench.render([result], page)
        self.assertEqual(page.read_text(), first)
        self.assertTrue(first.startswith("# Notes\n"))
        self.assertTrue(first.endswith("Footer\n"))
        self.assertIn("A/A noise row", first)
        self.assertIn("v1.1.0", first)

    def test_history_rejects_incomplete_or_misnamed_data(self):
        path = self.directory / "v1.0.0.json"
        record = {"schema_version": 1, "version": "v1.0.0", "complete": False}
        path.write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            bench.history(self.directory)
        record.update(complete=True, version="v1.1.0")
        path.write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            bench.history(self.directory)

    def test_comparison_unavailable_never_reports_a_pass(self):
        self.plan.update(baseline=None, baseline_state="suite_changed")
        self.suite["workloads"][0]["command"] = [sys.executable, "-c",
                                                "print('METRIC rate 100'); print('METRIC latency 10')"]
        with contextlib.redirect_stdout(io.StringIO()):
            result = bench.collect(self.plan, self.suite, "candidate", None, None, self.directory, 3)
        self.assertTrue(result["complete"])
        self.assertEqual(result["status"], "comparison_unavailable")

    def test_cli_rejects_zero_rounds_before_measuring(self):
        self.plan.update(suite_fingerprint=bench.load_suite()[1], baseline=None)
        path = self.directory / "plan.json"
        path.write_text(json.dumps(self.plan))
        proc = subprocess.run(
            [sys.executable, str(bench.ROOT / "tools/maint/release_bench.py"), "collect",
             "--plan", str(path), "--out", "candidate", "--history", str(self.directory / "history"),
             "--output-dir", str(self.directory / "output"), "--rounds", "0"],
            text=True, capture_output=True)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("at least three rounds", proc.stderr)
        self.assertFalse((self.directory / "output").exists())

    def test_dev_preview_can_render_without_published_data(self):
        self.plan["version"] = "dev-" + "b" * 12
        result = self.collect_fixture("print('METRIC rate 100'); print('METRIC latency 10')", baseline=False)
        page = self.directory / "preview" / "docs" / "benchmarks.md"
        page.parent.mkdir(parents=True)
        page.write_text(f"Notes\n{bench.START}\n{bench.END}\n")
        records = bench.history(self.directory / "preview" / "bench" / "results")
        bench.render(records + [result], page)
        self.assertIn("dev-" + "b" * 12, page.read_text())


if __name__ == "__main__":
    unittest.main()
