from pathlib import Path
import shutil
import subprocess
import unittest
from unittest.mock import patch
import uuid

import change_scope as scope


class ChangeScopeTest(unittest.TestCase):
    def decide(self, data):
        return scope.classify(scope.parse_diff(data))

    def test_allowlisted_docs(self):
        for path in (*scope.SAFE_DOCS, "changelog.d/123-fix.md", "changelog.d/README.md"):
            result = self.decide(b"M\0" + path.encode() + b"\0")
            self.assertTrue(result["docs_only"], path)

    def test_unknown_and_contract_docs_run_full(self):
        for path in (
            "docs/durability.md", "docs/PROTOCOL.md", "docs/teardown-design.md",
            "docs/new.md", "README.rst", "examples/README.md", "changelog.d/nested/123.md",
            ".github/workflows/ci.yml", "bootstrap.sh", "orly/code.cc",
        ):
            self.assertFalse(self.decide(b"M\0" + path.encode() + b"\0")["docs_only"], path)

    def test_clients_keep_full_coverage(self):
        result = self.decide(b"M\0clients/ts/src/index.ts\0A\0clients/smoke/test.sh\0")
        self.assertEqual("clients-only", result["scope"])
        self.assertFalse(result["docs_only"])

    def test_mixed_changes_run_full(self):
        result = self.decide(b"M\0README.md\0M\0orly/code.cc\0")
        self.assertEqual("full", result["scope"])

    def test_empty_diff_runs_full(self):
        self.assertFalse(self.decide(b"")["docs_only"])

    def test_renames_and_copies_consider_both_paths(self):
        for status in (b"R100", b"C100"):
            self.assertFalse(self.decide(status + b"\0orly/code.cc\0README.md\0")["docs_only"])
            self.assertFalse(self.decide(status + b"\0README.md\0orly/code.cc\0")["docs_only"])
            self.assertTrue(self.decide(status + b"\0README.md\0CONTRIBUTING.md\0")["docs_only"])

    def test_deletions_retain_original_paths(self):
        self.assertFalse(self.decide(b"D\0orly/code.cc\0")["docs_only"])
        self.assertTrue(self.decide(b"D\0README.md\0")["docs_only"])

    def test_malformed_diff_is_not_guessed(self):
        for data in (
            b"M\0README.md", b"R100\0README.md\0", b"X\0README.md\0",
            b"T\0README.md\0", b"U\0README.md\0", b"R101\0README.md\0CONTRIBUTING.md\0",
            b"M\0../README.md\0", b"M\0/README.md\0", b"M\0bad\xff.md\0",
        ):
            with self.assertRaises((ValueError, UnicodeError)):
                scope.parse_diff(data)

    def test_non_pr_and_missing_sha_run_full(self):
        for event in ("push", "schedule", "workflow_dispatch", "release"):
            self.assertFalse(scope.classify_repository(Path.cwd(), "a" * 40, "b" * 40, event)["docs_only"])
        self.assertFalse(scope.classify_repository(Path.cwd(), None, "b" * 40, "pull_request")["docs_only"])

    def test_git_failure_runs_full(self):
        with patch.object(scope, "git", side_effect=OSError("unavailable")):
            self.assertFalse(scope.classify_repository(Path.cwd(), "a" * 40, "b" * 40, "pull_request")["docs_only"])


class GitDiffTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path.cwd() / ".ci-checks" / f"scope-fixture-{uuid.uuid4().hex}"
        self.repo.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.repo)
        self.git("init", "-q", "-b", "master")
        self.git("config", "user.name", "Patrick O'Reilly")
        self.git("config", "user.email", "p@ovpl.co")
        self.git("config", "commit.gpgsign", "false")
        (self.repo / "README.md").write_text("base\n")
        (self.repo / "CONTRIBUTING.md").write_text("contributing\n")
        (self.repo / "code.cc").write_text("code\n")
        self.commit()
        self.base = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "-b", "ohohoreilly/scope-fixture")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.STDOUT, text=True).strip()

    def commit(self):
        self.git("add", "README.md", "CONTRIBUTING.md", "code.cc")
        self.git("commit", "-qm", "scope validation fixture")

    def merge(self):
        self.git("checkout", "-q", "master")
        self.git("merge", "-q", "--no-ff", "--no-edit", "ohohoreilly/scope-fixture")
        return self.git("rev-parse", "HEAD")

    def classify(self):
        return scope.classify_repository(self.repo, self.base, self.merge(), "pull_request")

    def test_complete_pr_not_just_last_commit(self):
        (self.repo / "code.cc").write_text("changed code\n")
        self.commit()
        (self.repo / "README.md").write_text("latest docs\n")
        self.commit()
        self.assertFalse(self.classify()["docs_only"])

    def test_real_docs_merge(self):
        (self.repo / "README.md").write_text("docs change\n")
        self.commit()
        self.assertTrue(self.classify()["docs_only"])

    def test_real_code_to_docs_rename(self):
        self.git("rm", "-q", "CONTRIBUTING.md")
        self.git("mv", "code.cc", "CONTRIBUTING.md")
        self.git("add", "CONTRIBUTING.md")
        self.git("commit", "-qm", "scope validation rename")
        self.assertFalse(self.classify()["docs_only"])

    def test_symlink_doc_runs_full(self):
        (self.repo / "README.md").unlink()
        (self.repo / "README.md").symlink_to("code.cc")
        self.commit()
        self.assertFalse(self.classify()["docs_only"])

    def test_executable_doc_runs_full(self):
        (self.repo / "README.md").chmod(0o755)
        self.commit()
        self.assertFalse(self.classify()["docs_only"])

    def test_new_symlink_fragment_runs_full(self):
        (self.repo / "changelog.d").mkdir()
        (self.repo / "changelog.d" / "123-docs.md").symlink_to("../code.cc")
        self.git("add", "changelog.d/123-docs.md")
        self.git("commit", "-qm", "scope validation new symlink")
        self.assertFalse(self.classify()["docs_only"])

    def test_real_doc_deletion(self):
        self.git("rm", "-q", "README.md")
        self.git("commit", "-qm", "scope validation deletion")
        self.assertTrue(self.classify()["docs_only"])

    def test_base_and_checkout_mismatches_run_full(self):
        (self.repo / "README.md").write_text("docs\n")
        self.commit()
        tested = self.merge()
        self.assertFalse(scope.classify_repository(self.repo, "a" * 40, tested, "pull_request")["docs_only"])
        self.git("checkout", "-q", self.base)
        self.assertFalse(scope.classify_repository(self.repo, self.base, tested, "pull_request")["docs_only"])

    def test_not_tested_merge_runs_full(self):
        (self.repo / "README.md").write_text("docs\n")
        self.commit()
        head = self.git("rev-parse", "HEAD")
        self.assertFalse(scope.classify_repository(self.repo, self.base, head, "pull_request")["docs_only"])


if __name__ == "__main__":
    unittest.main()
