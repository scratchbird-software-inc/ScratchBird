# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest import mock
from owned_test_runtime import (CleanupRefused, OwnedTestRuntime, _active_references,
                                _all_threads_terminal, _process_exit_confirmed)


class RuntimeCleanupTest(unittest.TestCase):
    def test_inaccessible_process_requires_verified_disappearance(self):
        for final_state in (FileNotFoundError(), ProcessLookupError(),
                            SimpleNamespace(st_uid=os.getuid())):
            with self.subTest(final_state=final_state):
                process = mock.MagicMock()
                process.name = str(os.getpid() + 1)
                process.stat.side_effect = [SimpleNamespace(st_uid=os.getuid()), final_state]
                process.__truediv__.return_value.read_bytes.side_effect = PermissionError()
                with mock.patch("owned_test_runtime.Path.iterdir", return_value=iter([process])), \
                     mock.patch("owned_test_runtime._process_exit_confirmed", return_value=False):
                    if isinstance(final_state, BaseException):
                        self.assertEqual(_active_references(Path("/fixture-runtime")), [])
                    else:
                        with self.assertRaises(CleanupRefused):
                            _active_references(Path("/fixture-runtime"))
                self.assertEqual(process.stat.call_count, 2)

    def test_process_lifetime_handle_distinguishes_live_from_exited(self):
        child = subprocess.Popen([sys.executable, "-c",
                                  "import sys; print('ready', flush=True); sys.stdin.read()"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), "ready")
            process = Path("/proc") / str(child.pid)
            self.assertFalse(_process_exit_confirmed(process))
            child.stdin.close()
            # EOF proves the child has closed stdout; do not reap it yet. The
            # pidfd observes exit and the terminal proc entry is checked too.
            self.assertEqual(child.stdout.read(), "")
            self.assertTrue(_process_exit_confirmed(process))
        finally:
            if not child.stdin.closed:
                child.stdin.close()
            child.wait(timeout=5)
            child.stdout.close()

    def test_terminal_handle_does_not_excuse_reused_or_unreadable_pid(self):
        process = mock.MagicMock()
        process.name = "42"
        for contents, expected in (("42 (exited worker) Z 1 2", True),
                                   ("42 (reused worker) S 1 2", False),
                                   ("invalid", False), (PermissionError(), False)):
            with self.subTest(contents=contents):
                reader = process.__truediv__.return_value.read_text
                reader.side_effect = contents if isinstance(contents, BaseException) else None
                reader.return_value = contents
                with mock.patch("owned_test_runtime.os.pidfd_open", return_value=91, create=True), \
                     mock.patch("owned_test_runtime.os.close") as close, \
                     mock.patch("owned_test_runtime.select.poll") as poll:
                    poll.return_value.poll.return_value = [(91, 1)]
                    self.assertEqual(_process_exit_confirmed(process), expected)
                    close.assert_called_once_with(91)

    def test_exit_confirmed_without_pidfd_open(self):
        # Interpreters without os.pidfd_open must still confirm a real zombie
        # and must still refuse a live process.
        child = subprocess.Popen([sys.executable, "-c",
                                  "import sys; print('ready', flush=True); sys.stdin.read()"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), "ready")
            process = Path("/proc") / str(child.pid)
            with mock.patch("owned_test_runtime.os.pidfd_open", side_effect=AttributeError,
                            create=True):
                self.assertFalse(_process_exit_confirmed(process))
                child.stdin.close()
                self.assertEqual(child.stdout.read(), "")
                deadline = time.monotonic() + 5
                while not _process_exit_confirmed(process) and time.monotonic() < deadline:
                    pass
                self.assertTrue((process / "stat").read_text().split(") ")[1].startswith("Z"))
                self.assertTrue(_process_exit_confirmed(process))
        finally:
            if not child.stdin.closed:
                child.stdin.close()
            child.wait(timeout=5)
            child.stdout.close()

    def test_zombie_leader_with_live_thread_is_not_terminal(self):
        process = mock.MagicMock()
        process.name = "42"
        leader, worker = mock.MagicMock(), mock.MagicMock()
        process.__truediv__.side_effect = lambda name: {
            "stat": mock.MagicMock(read_text=mock.MagicMock(return_value="42 (w) Z 1 2")),
            "task": mock.MagicMock(iterdir=mock.MagicMock(return_value=[leader, worker])),
        }[name]
        leader.__truediv__.return_value.read_text.return_value = "42 (w) Z 1 2"
        worker.__truediv__.return_value.read_text.return_value = "43 (w) S 1 2"
        self.assertFalse(_all_threads_terminal(process, wait_seconds=0))
        worker.__truediv__.return_value.read_text.return_value = "43 (w) X 1 2"
        self.assertTrue(_all_threads_terminal(process, wait_seconds=0))

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

    # The runtime creator runs in its own process so "descendant of the
    # creator" and "unrelated" are real process-tree facts, not mocks.
    CREATOR = """
import ctypes, os, sys, time
sys.path.insert(0, {support!r})
from owned_test_runtime import CleanupRefused, OwnedTestRuntime
runtime = OwnedTestRuntime({parent!r})
daemon = 0
if {daemon!r}:
    read_end, write_end = os.pipe()
    if os.fork() == 0:
        grandchild = os.fork()
        if grandchild == 0:
            ctypes.CDLL(None).prctl(4, 0, 0, 0, 0)  # PR_SET_DUMPABLE 0
            time.sleep(60)
            os._exit(0)
        os.write(write_end, str(grandchild).encode())
        os._exit(0)
    os.wait()
    daemon = int(os.read(read_end, 32))
print(daemon, runtime.path, flush=True)
sys.stdin.readline()
try:
    runtime.cleanup({evidence!r}, controller_finished=True)
    print("removed", flush=True)
except CleanupRefused as error:
    print("refused", error, flush=True)
"""

    def _run_creator(self, parent, daemon, unrelated):
        support = str(Path(__file__).resolve().parent)
        creator = subprocess.Popen(
            [sys.executable, "-I", "-c", self.CREATOR.format(
                support=support, parent=parent, daemon=daemon,
                evidence=str(Path(parent) / "evidence"))],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        stranger = None
        try:
            daemon, _, runtime_path = creator.stdout.readline().strip().partition(" ")
            runtime_path = Path(runtime_path)
            self.assertTrue(runtime_path.is_dir())
            if unrelated:
                # Started after the runtime by a process outside the creator's
                # tree, never told the path, and not inspectable via procfs.
                stranger = subprocess.Popen(
                    [sys.executable, "-I", "-c",
                     "import ctypes,sys,time; ctypes.CDLL(None).prctl(4,0,0,0,0); "
                     "print('ready', flush=True); time.sleep(60)"],
                    stdout=subprocess.PIPE, text=True)
                self.assertEqual(stranger.stdout.readline().strip(), "ready")
                with self.assertRaises(PermissionError):
                    Path("/proc", str(stranger.pid), "environ").read_bytes()
            creator.stdin.write("go\n")
            creator.stdin.flush()
            outcome = creator.stdout.readline().strip()
            return runtime_path, outcome, int(daemon)
        finally:
            if stranger:
                stranger.kill()
                stranger.wait()
                stranger.stdout.close()
            creator.stdin.close()
            creator.wait(timeout=30)
            creator.stdout.close()

    def test_uninspectable_unrelated_process_does_not_block_cleanup(self):
        with tempfile.TemporaryDirectory() as parent:
            runtime_path, outcome, _ = self._run_creator(parent, daemon=False, unrelated=True)
            self.assertEqual(outcome, "removed")
            self.assertFalse(runtime_path.exists())

    def test_uninspectable_daemonized_descendant_still_refuses(self):
        with tempfile.TemporaryDirectory() as parent:
            runtime_path, outcome, daemon = self._run_creator(parent, daemon=True, unrelated=False)
            try:
                self.assertTrue(outcome.startswith("refused cannot inspect an owned process"),
                                outcome)
                self.assertTrue(runtime_path.exists())
            finally:
                # The double-forked daemon outlives its creator; end it here,
                # then remove the runtime the refusal correctly left behind.
                if daemon:
                    try:
                        os.kill(daemon, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                shutil.rmtree(runtime_path, ignore_errors=True)

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
