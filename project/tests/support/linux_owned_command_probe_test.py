#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import linux_owned_command_probe as probe

PROBE = Path(__file__).with_name("linux_owned_command_probe.py")


class OwnedCommandProbeTest(unittest.TestCase):
    def test_exited_process_task_directory_has_no_live_children(self):
        for error in (FileNotFoundError("exited"), ProcessLookupError("exited during enumeration")):
            with self.subTest(error=type(error).__name__), mock.patch.object(Path, "iterdir", side_effect=error):
                self.assertEqual(probe.children(123), set())

    def test_exited_task_children_file_does_not_abort_sampling(self):
        for error in (FileNotFoundError("exited"), ProcessLookupError("exited during read")):
            with self.subTest(error=type(error).__name__), \
                    mock.patch.object(Path, "iterdir", return_value=iter([Path("/proc/123/task/123")])), \
                    mock.patch.object(Path, "read_text", side_effect=error):
                self.assertEqual(probe.children(123), set())

    def test_child_enumeration_permissions_and_malformed_data_fail_closed(self):
        with mock.patch.object(Path, "iterdir", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                probe.children(123)
        with mock.patch.object(Path, "iterdir", return_value=iter([Path("/proc/123/task/123")])), \
                mock.patch.object(Path, "read_text", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                probe.children(123)
        with mock.patch.object(Path, "iterdir", return_value=iter([Path("/proc/123/task/123")])), \
                mock.patch.object(Path, "read_text", return_value="bad pid"):
            with self.assertRaises(ValueError):
                probe.children(123)

    def test_child_enumeration_unions_all_tasks_without_duplicates(self):
        with mock.patch.object(Path, "iterdir", return_value=iter([Path("task1"), Path("task2")])), \
                mock.patch.object(Path, "read_text", side_effect=["2 3", "3 4"]):
            self.assertEqual(probe.children(123), {2, 3, 4})

    def test_unavailable_io_is_recorded_not_reported_as_zero(self):
        original = Path.read_bytes
        def read(path):
            if path == Path(f"/proc/{os.getpid()}/io"):
                raise PermissionError("transient procfs permission")
            return original(path)
        with mock.patch.object(Path, "read_bytes", read):
            row = probe.snapshot(os.getpid())
        self.assertIsNone(row["io"])
        self.assertEqual(row["unavailable"]["io"], "PermissionError")
        self.assertEqual(row["pid"], os.getpid())

    def run_probe(self, code, timeout=10):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence"
            result = subprocess.run([sys.executable, "-B", str(PROBE),
                "--evidence", str(evidence), "--timeout", str(timeout),
                "--", sys.executable, "-c", code], capture_output=True, text=True, timeout=20)
            self.assertTrue((evidence / "receipt.json").exists(), result.stderr)
            receipt = json.loads((evidence / "receipt.json").read_text())
            samples = [json.loads(line) for line in (evidence / "process-samples.jsonl").read_text().splitlines()]
            return result, receipt, samples, (evidence / "command.log").read_text()

    def test_success_has_resource_receipt_and_output(self):
        result, receipt, samples, output = self.run_probe("print('owned output')")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(receipt["status"], "PASS")
        self.assertEqual(receipt["leaked_descendants"], [])
        self.assertGreater(receipt["peak_child_rss_kib"], 0)
        self.assertGreaterEqual(receipt["elapsed_seconds"], 0)
        self.assertTrue(samples)
        self.assertEqual(output, "owned output\n")

    def test_nonzero_exit_is_not_masked(self):
        result, receipt, _, _ = self.run_probe("raise SystemExit(29)")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(receipt["status"], "FAILED")
        self.assertEqual(receipt["exit_code"], 29)

    def test_prior_reaped_children_do_not_pollute_command_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence"
            warmup = "import time; memory=bytearray(96*1024*1024); end=time.process_time()+0.5\nwhile time.process_time()<end: pass"
            launcher = ("import os,subprocess,sys; "
                        "subprocess.run([sys.executable,'-c',sys.argv[1]],check=True); "
                        "os.execv(sys.executable,[sys.executable,'-B',*sys.argv[2:]])")
            result = subprocess.run([sys.executable, "-B", "-c", launcher, warmup,
                str(PROBE), "--evidence", str(evidence), "--timeout", "10",
                "--", sys.executable, "-B", "-c", "print('measured only')"],
                capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            receipt = json.loads((evidence / "receipt.json").read_text())
            self.assertEqual(receipt["status"], "PASS")
            self.assertEqual(receipt["resource_accounting"], "wait4_command_and_adopted_descendants")
            self.assertLess(receipt["peak_child_rss_kib"], 64*1024)
            self.assertLess(receipt["user_cpu_seconds"] + receipt["system_cpu_seconds"], 0.5)

    def test_launch_failure_cannot_leave_a_running_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence"
            result = subprocess.run([sys.executable, "-B", str(PROBE),
                "--evidence", str(evidence), "--", str(Path(directory) / "missing-command")],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            receipt = json.loads((evidence / "receipt.json").read_text())
            self.assertEqual(receipt["status"], "FAILED")
            self.assertIn("FileNotFoundError", receipt["probe_error"])
            self.assertEqual(receipt["leaked_descendants"], [])

    def test_timeout_stops_owned_command(self):
        result, receipt, _, _ = self.run_probe("import time; time.sleep(60)", timeout=0.1)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(receipt["timed_out"])
        self.assertLess(receipt["elapsed_seconds"], 10)

    def test_timeout_kills_command_that_ignores_termination(self):
        result, receipt, _, _ = self.run_probe(
            "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)", timeout=0.5)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(receipt["timed_out"])
        self.assertEqual(receipt["exit_code"], -9)
        self.assertEqual(receipt["leaked_descendants"], [])
        self.assertGreater(receipt["peak_child_rss_kib"], 0)
        self.assertLess(receipt["elapsed_seconds"], 10)

    def test_detached_descendant_fails_and_unrelated_process_survives(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            code = "import os,time; pid=os.fork(); (os.setsid(),time.sleep(60)) if pid==0 else None"
            result, receipt, _, _ = self.run_probe(code)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(receipt["status"], "FAILED")
            self.assertEqual(receipt["exit_code"], 0)
            self.assertEqual(len(receipt["leaked_descendants"]), 1)
            self.assertIsNone(unrelated.poll())
            leaked = receipt["leaked_descendants"][0]
            self.assertFalse(Path(f'/proc/{leaked["pid"]}').exists())
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
