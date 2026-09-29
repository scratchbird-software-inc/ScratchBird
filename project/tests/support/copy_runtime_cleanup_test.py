# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""COPY fixture lifecycle only; not evidence of SQL or engine execution."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

WORKERS = Path(__file__).resolve().parent.parent / "sbsql_parser_worker"
sys.path.insert(0, str(WORKERS))
SPEC = importlib.util.spec_from_file_location(
    "copy_cleanup_subject", WORKERS / "sbsql_copy_persistence_full_route_gate.py")
GATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


class CopyRuntimeCleanupTest(unittest.TestCase):
    def check_lifecycle(self, failure):
        allocated = []
        runtime_type = GATE.OwnedTestRuntime

        def allocate(**kwargs):
            runtime = runtime_type(**kwargs)
            allocated.append(runtime.path)
            return runtime

        def lane(name, args, work, fixtures):
            root = work / name
            root.mkdir()
            (root / "copy.sbdb").write_bytes(b"partial generated fixture")
            (root / "copy.sbdb.sb.companion").write_bytes(b"generated companion")
            (root / "server.err").write_text("retained diagnostic\n")
            if failure == name:
                raise GATE.CopyPersistenceError("deliberate " + name + " failure")

        with tempfile.TemporaryDirectory(prefix="copy_cleanup_test_") as parent:
            reports = Path(parent) / ("reports_" + "x" * 100)
            fixtures = Path(parent) / "fixtures"
            fixtures.mkdir()
            if failure != "fixtures":
                for name in ("copy_persist.rows", "copy_rollback.rows", "copy_persist_expected.csv"):
                    (fixtures / name).write_text("fixture\n")
            args = ["copy", "--work-dir", str(reports), "--fixture-root", str(fixtures)]
            for option in ("server", "listener", "parser-worker", "sb-isql", "example-db-seeder", "openssl"):
                args += ["--" + option, sys.executable]
            with patch.object(GATE, "OwnedTestRuntime", side_effect=allocate), \
                 patch.object(GATE, "run_plain_copy_lane", side_effect=lambda *a: lane("plain", *a)) as plain, \
                 patch.object(GATE, "run_tls_copy_lane", side_effect=lambda *a: lane("tls", *a)) as tls, \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(GATE.main(args), 0 if failure is None else 1)
            self.assertEqual(plain.call_count, 0 if failure == "fixtures" else 1)
            self.assertEqual(tls.call_count, 0 if failure in ("fixtures", "plain") else 1)
            self.assertEqual(len(allocated), 1)
            self.assertFalse(allocated[0].exists())
            socket_path = allocated[0] / "plain/restart/lc" / ("sbsql_" + "0" * 32 + ".management.sock")
            self.assertLess(len(os.fsencode(socket_path)), 100)
            receipts = list(reports.rglob("runtime-cleanup.json"))
            self.assertEqual(len(receipts), 1)
            self.assertEqual(json.loads(receipts[0].read_text())["runtime"], str(allocated[0]))
            self.assertEqual(list(reports.rglob("*.sbdb*")), [])
            self.assertEqual(len(list(reports.rglob("server.err"))), plain.call_count + tls.call_count)

    def test_success_cleans_both_lanes(self):
        self.check_lifecycle(None)

    def test_plain_failure_cleans_partial_database(self):
        self.check_lifecycle("plain")

    def test_tls_failure_cleans_both_databases(self):
        self.check_lifecycle("tls")

    def test_missing_fixture_cleans_allocated_runtime(self):
        self.check_lifecycle("fixtures")


if __name__ == "__main__":
    unittest.main()
