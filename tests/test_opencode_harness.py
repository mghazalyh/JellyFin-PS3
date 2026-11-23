"""Host tests for OpenCode supervisor's independent, fail-closed semantics."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "opencode_harness.py"
spec = importlib.util.spec_from_file_location("opencode_harness", PATH)
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


class SupervisorTests(unittest.TestCase):
    def test_subprocess_success_and_sha(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            result = h.run([sys.executable, "-c", "print('hello')"],
                           base, 5, base / "success.log")
            self.assertTrue(result["ok"])
            self.assertEqual(result["reason"], "")
            self.assertGreater(result["stdout_bytes"], 0)
            self.assertEqual(len(result["stdout_sha256"]), 64)

    def test_subprocess_nonzero_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            result = h.run([sys.executable, "-c", "raise SystemExit(17)"],
                           base, 5, base / "failure.log")
            self.assertFalse(result["ok"])
            self.assertEqual(result["returncode"], 17)

    def test_subprocess_timeout_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            result = h.run([sys.executable, "-c", "import time; time.sleep(30)"],
                           base, 1, base / "timeout.log")
            self.assertFalse(result["ok"])
            self.assertEqual(result["reason"], "timeout")

    def test_output_limit_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            result = h.run([sys.executable, "-c",
                            "import sys,time; print('A'*200000,flush=True); time.sleep(30)"],
                           base, 4, base / "overflow.log", max_output=4096)
            self.assertFalse(result["ok"])
            self.assertEqual(result["reason"], "output_limit")

    def test_missing_executable_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            result = h.run(["__opencode_fake_unavailable__"],
                           base, 5, base / "missing.log")
            self.assertFalse(result["ok"])
            self.assertTrue(result["reason"].startswith("process_error:"))

    def test_git_branch_guard(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = pathlib.Path(folder) / "git"
            logs = pathlib.Path(folder) / "logs"
            repo.mkdir()
            subprocess.run(["git", "init", "-q", str(repo)],
                           check=True, capture_output=True)
            subprocess.run(["git", "checkout", "-qb", "main"],
                           cwd=repo, check=True, capture_output=True)
            with self.assertRaises(h.HarnessError):
                h.git_check(repo, logs)
            subprocess.run(["git", "checkout", "-qb",
                            "feature/ps3-unattended-harness-test"],
                           cwd=repo, check=True, capture_output=True)
            branch, status = h.git_check(repo, logs)
            self.assertTrue(branch.startswith(h.ALLOWED_BRANCH_PREFIX))
            self.assertEqual(status, "")

    def test_phase_failure_creates_report(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            repo = base / "git"
            repo.mkdir()
            subprocess.run(["git", "init", "-q", str(repo)],
                           check=True, capture_output=True)
            subprocess.run(["git", "checkout", "-qb",
                            "feature/ps3-unattended-harness-test"],
                           cwd=repo, check=True, capture_output=True)
            result = h.execute(repo, base / "logs", ["audit"],
                               "__opencode_fake_unavailable__", 5, 5)
            self.assertEqual(result["result"], "failed")
            saved = json.loads((base / "logs" / "report.json").read_text())
            self.assertEqual(saved["result"], "failed")
            self.assertFalse(saved["push_performed"])
            self.assertFalse(saved["deployment_performed"])

    def test_independent_validation_fail_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            with patch.object(h, "git_check", return_value=("feature/ps3-unattended-harness-test", "")):
                with patch.object(h, "independent_validation", return_value={
                    "python_unittest": {"ok": False, "reason": "regression"}
                }):
                    result = h.execute(base, base / "out", ["validate"],
                                       "opencode", 5, 5)
                    self.assertEqual(result["result"], "failed")


if __name__ == "__main__":
    unittest.main()
