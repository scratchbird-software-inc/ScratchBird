# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from owned_test_runtime import CleanupRefused, OwnedTestRuntime


class RuntimeCleanupTest(unittest.TestCase):
    def test_success_and_failure_keep_evidence_not_database(self):
        for exit_code in (0, 1):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as parent:
                runtime = OwnedTestRuntime(parent)
                result = subprocess.run([sys.executable, "-c", "raise SystemExit(" + str(exit_code) + ")"])
                self.assertEqual(result.returncode, exit_code)
                (runtime.path / "test.sbdb").write_bytes(b"database fixture")
                (runtime.path / "test.sbdb.sb.mga_rows").write_bytes(b"companion")
                (runtime.path / "run.log").write_text("failure details\n")
                (runtime.path / "fixture.sql").write_text("SELECT 1;\n")
                external = Path(parent) / "unrelated.sbdb"
                external.write_bytes(b"preserve me")
                (runtime.path / "external.log").symlink_to(external)
                receipt = runtime.cleanup(Path(parent) / "evidence", controller_finished=True)
                self.assertFalse(runtime.path.exists())
                self.assertGreater(receipt["allocated_bytes_removed"], 0)
                self.assertEqual(external.read_bytes(), b"preserve me")
                diagnostics = Path(parent) / "evidence/runtime-diagnostics"
                self.assertEqual((diagnostics / "run.log").read_text(), "failure details\n")
                self.assertEqual((diagnostics / "fixture.sql").read_text(), "SELECT 1;\n")
                self.assertFalse((diagnostics / "test.sbdb").exists())
                self.assertFalse((diagnostics / "external.log").exists())

    def test_controller_must_be_finished(self):
        with tempfile.TemporaryDirectory() as parent:
            runtime = OwnedTestRuntime(parent)
            with self.assertRaises(CleanupRefused):
                runtime.cleanup(Path(parent) / "evidence", controller_finished=False)
            self.assertTrue(runtime.path.exists())

    def test_live_environment_cwd_and_open_file_prevent_removal(self):
        for reference in ("env", "cwd", "fd"):
            with self.subTest(reference=reference), tempfile.TemporaryDirectory() as parent:
                runtime = OwnedTestRuntime(parent)
                database = runtime.path / "fixture.sbdb"
                database.write_bytes(b"keep while active")
                code = "import sys; print('ready', flush=True); sys.stdin.read()"
                options = {}
                handle = None
                if reference == "env":
                    options["env"] = dict(os.environ, TMPDIR=str(runtime.path))
                elif reference == "cwd":
                    options["cwd"] = runtime.path
                else:
                    handle = database.open("rb")
                    options["pass_fds"] = (handle.fileno(),)
                child = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, text=True, **options)
                try:
                    self.assertEqual(child.stdout.readline().strip(), "ready")
                    with self.assertRaises(CleanupRefused):
                        runtime.cleanup(Path(parent) / "evidence", controller_finished=True)
                    self.assertEqual(database.read_bytes(), b"keep while active")
                finally:
                    child.communicate(timeout=5)
                    if handle:
                        handle.close()
                runtime.cleanup(Path(parent) / "evidence", controller_finished=True)
                self.assertFalse(runtime.path.exists())

    def test_root_substitution_and_inside_destination_refused(self):
        with tempfile.TemporaryDirectory() as parent:
            runtime = OwnedTestRuntime(parent)
            with self.assertRaises(CleanupRefused):
                runtime.cleanup(runtime.path / "evidence", controller_finished=True)
            original = runtime.path.with_name("original")
            runtime.path.rename(original)
            runtime.path.mkdir(mode=0o700)
            with self.assertRaises(CleanupRefused):
                runtime.cleanup(Path(parent) / "evidence", controller_finished=True)
            self.assertTrue(original.is_dir())
            self.assertTrue(runtime.path.is_dir())

    def test_large_diagnostics_reported_not_archived(self):
        with tempfile.TemporaryDirectory() as parent:
            runtime = OwnedTestRuntime(parent)
            with (runtime.path / "large.log").open("wb") as output:
                output.truncate(5 * 1024 * 1024)
            receipt = runtime.cleanup(Path(parent) / "evidence", controller_finished=True)
            self.assertEqual(receipt["omitted_diagnostics"], [{"path": "large.log",
                "bytes": 5 * 1024 * 1024, "reason": "diagnostic retention budget"}])
            self.assertEqual(receipt["diagnostic_bytes_retained"], 0)


if __name__ == "__main__":
    unittest.main()
