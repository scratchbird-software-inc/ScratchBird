#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Independent corruption controls for the native-ingest result oracle."""

import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import cdp_native_bulk_ingest_cli_gate as gate


class RoundTripOracleTests(unittest.TestCase):
    def rows(self):
        # Independently reproduce the fixed input, including its duplicate
        # non-key values. This is CLI result data, never engine UUID authority.
        rows = []
        for row in range(1, 1601):
            customer = 1973 if row == 2 else 1000 + (row * 973) % 8000
            discount = 2428 if row == 2 else 1000 + (row * 428) % 8000
            optional = "(null)" if row % 101 == 0 else str(1000 + (row * 313) % 8000)
            rows.append(f"{row}|{customer}|{discount}|{optional}")
        return rows

    def check(self, rows, stderr="", returncode=0):
        gate.verify_round_trip([gate.RunResult(
            "control", "round_trip", returncode, "\n".join(rows), stderr)])

    def test_all_rows_with_distinct_null_rendering(self):
        rows = self.rows()
        self.assertEqual(sum(row.endswith("|(null)") for row in rows), 15)
        self.check(rows)

    def test_null_cannot_be_empty_zero_or_text_null(self):
        for replacement in ("", "0", "NULL"):
            rows = self.rows()
            rows[100] = rows[100].replace("(null)", replacement)
            with self.subTest(replacement=replacement), self.assertRaises(gate.NativeBulkIngestGateError):
                self.check(rows)

    def test_present_value_cannot_become_null(self):
        rows = self.rows()
        rows[0] = "1|1973|1428|(null)"
        with self.assertRaises(gate.NativeBulkIngestGateError):
            self.check(rows)

    def test_missing_duplicate_reordered_and_extra_rows_refused(self):
        rows = self.rows()
        for candidate in (rows[:-1], rows[:100] + rows[101:], rows + [rows[-1]],
                          [rows[1], rows[0]] + rows[2:], [rows[0]] + rows[:-1]):
            with self.subTest(size=len(candidate)), self.assertRaises(gate.NativeBulkIngestGateError):
                self.check(candidate)

    def test_failed_command_or_diagnostic_is_not_success(self):
        for stderr, code in (("error", 0), ("", 1)):
            with self.subTest(stderr=stderr, code=code), self.assertRaises(gate.NativeBulkIngestGateError):
                self.check(self.rows(), stderr, code)


class RouteOwnershipTests(unittest.TestCase):
    def test_partial_startup_is_owned_before_readiness(self):
        args = SimpleNamespace(database_seed="seed", resource_seed_pack_root="pack",
                               server="server", listener="listener", parser_worker="parser")
        for method, failure, expected in (
            (gate.start_local_ipc, "path", 1),
            (gate.start_inet, "path", 1),
            (gate.start_inet, "tcp", 2),
        ):
            processes = []
            created = [Mock(), Mock()]
            with self.subTest(method=method.__name__, failure=failure), \
                    patch.object(Path, "mkdir"), patch.object(Path, "open"), \
                    patch.object(gate, "seed_database"), \
                    patch.object(gate, "find_free_port", return_value=1234), \
                    patch.object(gate.subprocess, "Popen", side_effect=created), \
                    patch.object(gate, "wait_for_path", side_effect=RuntimeError("not ready") if failure == "path" else None), \
                    patch.object(gate, "wait_for_tcp", side_effect=RuntimeError("not ready")):
                with self.assertRaisesRegex(RuntimeError, "not ready"):
                    method(args, Path("unused"), processes)
                self.assertEqual(processes, created[:expected])

    def test_failed_route_joins_partial_start_before_returning(self):
        proc = Mock()

        def fail(args, work, processes):
            processes.append(proc)
            raise RuntimeError("not ready")

        with patch.object(gate, "write_native_rows"), \
                patch.object(gate, "run_embedded"), \
                patch.object(gate, "start_local_ipc", side_effect=fail), \
                patch.object(gate, "stop_process") as stop:
            with self.assertRaisesRegex(RuntimeError, "not ready"):
                gate.run_gate(SimpleNamespace(), Path("unused"))
            stop.assert_called_once_with(proc)

    def test_shutdown_failure_attempts_every_process(self):
        processes = [Mock(), Mock(), Mock()]

        def fail(args, work, retained):
            retained.extend(processes)
            raise RuntimeError("startup failure")

        with patch.object(gate, "write_native_rows"), \
                patch.object(gate, "run_embedded"), \
                patch.object(gate, "start_local_ipc", side_effect=fail), \
                patch.object(gate, "stop_process", side_effect=[RuntimeError("wait failed"), None, None]) as stop:
            with self.assertRaises(gate.RouteShutdownError):
                gate.run_gate(SimpleNamespace(), Path("unused"))
            self.assertEqual([call.args[0] for call in stop.call_args_list], list(reversed(processes)))

    def test_unresolved_shutdown_or_live_descendant_preserves_workspace(self):
        argv = ["gate", "--server", "s", "--listener", "l", "--parser-worker", "p",
                "--sb-isql", "c", "--database-seed", "d", "--resource-seed-pack-root", "r",
                "--work-dir", "evidence"]
        for startup_failure in (True, False):
            runtime = Mock(path=Path("unused"))
            if not startup_failure:
                runtime.cleanup.side_effect = gate.CleanupRefused("live descendant")
            with self.subTest(startup_failure=startup_failure), \
                    patch.object(gate, "OwnedTestRuntime", return_value=runtime), \
                    patch.object(gate, "run_gate", side_effect=gate.RouteShutdownError("unresolved") if startup_failure else None), \
                    patch.object(gate, "dump_logs"), patch("builtins.print"):
                self.assertEqual(gate.main(argv), 1)
                if startup_failure:
                    runtime.cleanup.assert_not_called()
                else:
                    runtime.cleanup.assert_called_once()


if __name__ == "__main__":
    unittest.main()
