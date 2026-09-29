# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Benchmark fixture lifecycle only; not evidence of database execution."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "benchmark_cleanup_subject",
    REPO / "project/tests/sbsql_parser_worker/legacy_execution_plan10_live_scratchbird_benchmark_gate.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class BenchmarkRuntimeCleanupTest(unittest.TestCase):
    def check_failure(self, phase):
        runtime_paths = []
        real_runtime = GATE.OwnedTestRuntime

        def allocate(**kwargs):
            owned = real_runtime(**kwargs)
            runtime_paths.append(owned.path)
            return owned

        def failed_seed(command, **kwargs):
            # The seeder can leave a database and companions before failing.
            # Only fixture cleanup is under test; no engine result is mocked.
            database = Path(command[1])
            database.write_bytes(b"incomplete test database")
            database.with_suffix(".sbdb.sb.partial").write_bytes(b"partial companion")
            raise GATE.LiveBenchmarkError("deliberate seed failure")

        with tempfile.TemporaryDirectory(prefix="benchmark_cleanup_test_") as parent:
            work = Path(parent) / "reports"
            args = ["benchmark", "--repo-root", str(REPO), "--work-dir", str(work)]
            for option in ("server", "listener", "parser-worker", "example-db-seeder", "sb-isql", "openssl"):
                args += ["--" + option, sys.executable]
            certificate_error = GATE.LiveBenchmarkError("deliberate certificate failure")
            with patch.object(GATE, "OwnedTestRuntime", side_effect=allocate), \
                 patch.object(GATE, "generate_server_cert",
                              side_effect=certificate_error if phase == "certificate" else None,
                              return_value=(work / "cert.pem", work / "key.pem")), \
                 patch.object(GATE, "run", side_effect=failed_seed) as seed, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(GATE.main(args), 1)
            self.assertEqual(seed.call_count, 0 if phase == "certificate" else 1)
            self.assertEqual(len(runtime_paths), 1)
            self.assertFalse(runtime_paths[0].exists())
            receipts = list(work.rglob("runtime-cleanup.json"))
            self.assertEqual(len(receipts), 1)
            receipt = json.loads(receipts[0].read_text())
            self.assertEqual(receipt["runtime"], str(runtime_paths[0]))
            self.assertEqual(list(work.rglob("*.sbdb*")), [])

    def test_certificate_failure_cleans_runtime(self):
        self.check_failure("certificate")

    def test_seeder_failure_cleans_partial_database(self):
        self.check_failure("seeder")


if __name__ == "__main__":
    unittest.main()
