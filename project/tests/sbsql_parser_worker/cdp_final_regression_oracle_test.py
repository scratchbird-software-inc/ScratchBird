#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Controller tests only; synthetic evidence is not product qualification."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import cdp_final_regression_benchmark_gate as gate


class ComponentEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="cdp050-controls-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.artifact = self.root / "fixture.json"

    def run_child(self, source, key="security_api_abi"):
        with patch.object(gate, "component_command", return_value=[sys.executable, "-c", source]), \
                contextlib.redirect_stdout(io.StringIO()):
            return gate.run_component(argparse.Namespace(), self.root, gate.Component(key, "unused"))

    def source(self, code=0):
        return f"import sys; print('output={self.artifact}'); sys.exit({code})"

    def valid_evidence(self):
        self.artifact.write_text(json.dumps({
            "schema_version": "controller.fixture.v1",
            "routes": ["embedded", "ipc", "inet"],
            "result_hash": "synthetic-controller-vector-only",
            "authority_boundary": {
                "parser_finality_authority": False,
                "reference_finality_authority": False,
                "finality_visibility_authority": "engine_mga",
            },
        }), encoding="utf-8")

    def test_success_requires_valid_evidence_and_retains_logs(self):
        self.valid_evidence()
        result = self.run_child(self.source())
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["json_artifact_path"], str(self.artifact))
        self.assertEqual(Path(result["stdout_path"]).read_text(), f"output={self.artifact}\n")
        self.assertTrue(Path(result["stderr_path"]).exists())
        self.assertGreaterEqual(result["elapsed_seconds"], 0)

    def test_zero_exit_without_artifact_is_failed(self):
        result = self.run_child("print('not evidence')")
        self.assertEqual(result["status"], "failed")
        self.assertIn("did not report", result["validation_failure"])
        self.assertEqual(result["returncode"], 0)

    def test_announced_but_absent_artifact_is_failed(self):
        result = self.run_child(self.source())
        self.assertEqual(result["status"], "failed")
        self.assertIn("missing", result["validation_failure"])

    def test_bad_json_is_failed_and_log_location_retained(self):
        self.artifact.write_text("not JSON", encoding="utf-8")
        result = self.run_child(self.source())
        self.assertEqual(result["status"], "failed")
        self.assertIn("malformed JSON", result["validation_failure"])
        self.assertTrue(Path(result["stdout_path"]).is_file())

    def test_bad_route_coverage_is_failed(self):
        self.valid_evidence()
        payload = json.loads(self.artifact.read_text())
        payload["routes"] = ["embedded"]
        self.artifact.write_text(json.dumps(payload), encoding="utf-8")
        result = self.run_child(self.source())
        self.assertEqual(result["status"], "failed")
        self.assertIn("route coverage", result["validation_failure"])

    def test_nonzero_exit_cannot_be_overridden_by_valid_artifact(self):
        self.valid_evidence()
        result = self.run_child(self.source(7))
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["returncode"], 7)

    def test_full_diagnostics_on_disk_bounded_summary(self):
        result = self.run_child(
            "import sys; print('x'*100000); sys.stderr.write('y'*12000); sys.exit(3)",
            key="no_execution_plan_dependency")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(Path(result["stdout_path"]).stat().st_size, 100001)
        self.assertEqual(Path(result["stderr_path"]).stat().st_size, 12000)
        self.assertEqual(result["stderr_tail"], "y"*4000)

    def test_large_output_does_not_hide_later_artifact(self):
        self.valid_evidence()
        result = self.run_child("print('x'*100000); " + self.source())
        self.assertEqual(result["status"], "passed")

    def test_forbidden_runtime_dependency_is_failed(self):
        self.valid_evidence()
        payload = json.loads(self.artifact.read_text())
        payload["path"] = "/".join(("docs", "execution-plans", "bad"))
        self.artifact.write_text(json.dumps(payload), encoding="utf-8")
        result = self.run_child(self.source())
        self.assertEqual(result["status"], "failed")
        self.assertIn("runtime execution_plan path", result["validation_failure"])


if __name__ == "__main__":
    unittest.main()
