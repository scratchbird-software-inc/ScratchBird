#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Check result classification only; this does not qualify sanitizer execution."""

from __future__ import annotations

import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "release"))
import public_sanitizer_static_analysis_gate as gate


def complete_profiles():
    return [
        {
            "profile": profile,
            "status": "passed",
            "commands": [
                {"label": label, "returncode": 0, "status": "passed"}
                for label in (f"configure_{profile}", f"build_probe_{profile}", f"run_probe_{profile}")
            ],
        }
        for profile in ("none", "asan-ubsan", "tsan")
    ]


class SanitizerResultContractTest(unittest.TestCase):
    def invoke_command(self, code, output, markers=()):
        result = subprocess.CompletedProcess(["probe"], code, stdout=output)
        with mock.patch.object(gate.subprocess, "run", return_value=result):
            with contextlib.redirect_stderr(io.StringIO()):
                return gate.run_command(["probe"], Path.cwd(), "fixture",
                                        allowed_failure_markers=markers)

    def test_zero_exit_is_passed(self):
        result = self.invoke_command(0, "probe passed")
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["returncode"], 0)
        self.assertNotIn("diagnostic", result)

    def test_unmatched_failures_and_signals_are_not_unavailability(self):
        for code in (1, 66, -6, -11):
            for markers in ((), ("FATAL: ThreadSanitizer: unexpected memory mapping",)):
                with self.subTest(code=code, markers=markers):
                    with self.assertRaises(SystemExit) as raised:
                        self.invoke_command(code, "unrelated failure", markers)
                    self.assertEqual(raised.exception.code, 1)

    def test_unavailability_requires_exact_observed_marker(self):
        marker = "FATAL: ThreadSanitizer: unexpected memory mapping"
        for code in (66, -6):
            with self.subTest(code=code):
                result = self.invoke_command(code, marker + " address", (marker,))
                self.assertEqual(result["status"], "runtime_unavailable_fail_closed")
                self.assertEqual(result["diagnostic"], "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE")
                self.assertEqual(result["matched_failure_marker"], marker)
                self.assertEqual(result["returncode"], code)

    def test_every_profile_requires_all_three_successful_commands(self):
        self.assertEqual(gate.profile_qualification_errors(complete_profiles()), [])
        for profile in range(3):
            for command in range(3):
                for defect in ("missing", "failed", "unavailable", "false_success"):
                    records = complete_profiles()
                    commands = records[profile]["commands"]
                    if defect == "missing":
                        commands.pop(command)
                    elif defect == "failed":
                        commands[command]["status"] = "failed"
                    elif defect == "unavailable":
                        commands[command]["status"] = "runtime_unavailable_fail_closed"
                    else:
                        commands[command]["returncode"] = 66
                    with self.subTest(profile=profile, command=command, defect=defect):
                        self.assertTrue(gate.profile_qualification_errors(records))

    def test_missing_duplicate_unknown_and_unqualified_profiles_fail(self):
        valid = complete_profiles()
        variants = ([], valid[:2], valid + [valid[0]], list(reversed(valid)))
        for records in variants:
            with self.subTest(records=records):
                self.assertTrue(gate.profile_qualification_errors(records))
        for profile in range(3):
            for field, value in (("profile", "unknown"), ("status", "runtime_unavailable_fail_closed")):
                records = complete_profiles()
                records[profile][field] = value
                with self.subTest(profile=profile, field=field):
                    self.assertTrue(gate.profile_qualification_errors(records))

    def test_cli_retains_receipt_but_never_prints_pass_for_unqualified_profile(self):
        for disposition in ("passed", "runtime_unavailable_fail_closed"):
            records = complete_profiles()
            records[-1]["status"] = disposition
            if disposition != "passed":
                records[-1]["commands"][-1].update(status=disposition, returncode=66)
            evidence = {"profile_builds": records, "evidence_sha256": "fixture"}
            with self.subTest(disposition=disposition), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                output = root / "evidence.json"
                argv = ["gate", "--repo-root", directory, "--project-root", directory,
                        "--build-root", directory, "--work-root", directory,
                        "--output", str(output)]
                stdout = io.StringIO()
                with mock.patch.object(sys, "argv", argv):
                    with mock.patch.object(gate, "build_evidence", return_value=copy.deepcopy(evidence)):
                        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                            if disposition == "passed":
                                self.assertEqual(gate.main(), 0)
                            else:
                                with self.assertRaises(SystemExit) as raised:
                                    gate.main()
                                self.assertEqual(raised.exception.code, 1)
                self.assertEqual(json.loads(output.read_text(encoding="utf-8")), evidence)
                self.assertEqual("public_sanitizer_static_analysis_gate=passed" in stdout.getvalue(),
                                 disposition == "passed")


if __name__ == "__main__":
    unittest.main()
