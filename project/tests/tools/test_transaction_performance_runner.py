# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Workload/parser tests only, not live performance or durability evidence."""

import importlib.util
import hashlib
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1] / "sbsql_parser_worker"
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("transaction_performance_runner", ROOT / "transaction_performance_runner.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class TransactionPerformanceRunnerTest(unittest.TestCase):
    def test_history_is_separate_untraced_and_requires_all_acknowledgements(self):
        args = SimpleNamespace(history_commits=2, traced=True, sync_tracer=Path("/usr/bin/strace"))
        output = RUNNER.BEGIN + "\n" + "".join(
            f"{RUNNER.COMMAND}{i}\n{RUNNER.COMMIT_ACK}\n" for i in (1, 2)) + RUNNER.END + "\n"
        results = [{"stdout": output, "script_sha256": "fixture", "process_wall_seconds": 1.5},
                   {"stdout": "0\n"}]
        with patch.object(RUNNER, "run_sql", side_effect=results) as run:
            evidence = RUNNER.prepare_history(args, Path("db"), Path("evidence"))
            self.assertEqual(evidence["requested_commits"], 2)
            self.assertIsNone(evidence["retained_inventory_entries"])
            self.assertEqual(run.call_count, 2)
            for call in run.call_args_list:
                self.assertEqual(call.kwargs, {})  # tracing defaults stay off
                self.assertEqual(call.args[-1], "explicit")
            self.assertNotIn("BEGIN;", run.call_args_list[0].args[-2])
        with patch.object(RUNNER, "run_sql", return_value={"stdout": output.replace(RUNNER.COMMIT_ACK, "", 1)}):
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.prepare_history(args, Path("db"), Path("evidence"))
        with patch.object(RUNNER, "run_sql", side_effect=[results[0], {"stdout": "1\n"}]):
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.prepare_history(args, Path("db"), Path("evidence"))

    def test_zero_history_does_not_add_a_connection_or_transaction(self):
        with patch.object(RUNNER, "run_sql") as run:
            result = RUNNER.prepare_history(SimpleNamespace(history_commits=0), Path("db"), Path("ev"))
            self.assertEqual(result["status"], "not_requested")
            run.assert_not_called()
        with self.assertRaises(RUNNER.BenchmarkError):
            RUNNER.prepare_history(SimpleNamespace(history_commits=-1), Path("db"), Path("ev"))

    def test_empty_commit_control_has_no_hidden_begin_or_insert(self):
        sql, operations = RUNNER.empty_commit_workload(3, 1, "explicit")
        self.assertEqual(operations, ["commit"] * 3)
        self.assertEqual(sql.count("COMMIT;"), 3)
        self.assertNotIn("INSERT", sql)
        self.assertNotIn("BEGIN;", sql)
        self.assertTrue(sql.endswith(f"\\echo {RUNNER.END}\nROLLBACK;\n"))
        output = RUNNER.BEGIN + "\n" + "".join(
            f"{RUNNER.COMMAND}{i}\n{RUNNER.COMMIT_ACK}\n" for i in range(1, 4)) + RUNNER.END + "\n"
        self.assertEqual(len(RUNNER.execution_samples(output, operations)), 3)
        for count, batch, mode in [(0, 1, "explicit"), (-1, 1, "explicit"),
                                   (1, 2, "explicit"), (1, 1, "autocommit")]:
            with self.subTest(count=count, batch=batch, mode=mode), self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.empty_commit_workload(count, batch, mode)

    def test_empty_table_requires_actual_count_result(self):
        self.assertEqual(len(RUNNER.verify_empty_table("0\n")), 64)
        for text in ["", "\n", "1\n", "0\n0\n", "0\nError: failed\n", "COUNT(*)\n0\n"]:
            with self.subTest(text=text), self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.verify_empty_table(text)

    def test_sync_wrapper_is_scoped_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            command = RUNNER.sync_command(Path("/usr/bin/strace"), directory, ["cli", "fixture"])
            self.assertIn("--kill-on-exit", command)
            self.assertIn("trace=fsync,fdatasync", command)
            self.assertEqual(command[-3:], ["--", "cli", "fixture"])
            (directory / "sync.strace").write_text("preserve")
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.sync_command(Path("/usr/bin/strace"), directory, ["cli"])

    def test_sync_failure_evidence_retained_and_missing_trace_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.collect_sync_trace(directory)
            (directory / "sync.strace").write_text(
                "10  1780000000.000001 fsync(4</db>) = -1 EIO (Input/output error) <0.001000>\n")
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.collect_sync_trace(directory)
            self.assertEqual(json.loads((directory / "sync-summary.json").read_text())["failed_syscall_count"], 1)

    def test_explicit_commit_count_and_no_hidden_begin(self):
        sql, operations = RUNNER.workload(5, 2, "explicit")
        self.assertEqual(operations.count("insert"), 5)
        self.assertEqual(operations.count("commit"), 3)
        self.assertNotIn("BEGIN;", sql)
        self.assertIn("VALUES (5, 15)", sql)

    def test_autocommit_does_not_inject_commit_or_batch(self):
        sql, operations = RUNNER.workload(3, 1, "autocommit")
        self.assertEqual(operations, ["insert"] * 3)
        self.assertNotIn("COMMIT;", sql)
        self.assertEqual(sql.count("\\echo " + RUNNER.COMMAND), 3)
        self.assertTrue(sql.endswith(f"\\echo {RUNNER.END}\nROLLBACK;\n"))
        with self.assertRaises(RUNNER.BenchmarkError):
            RUNNER.workload(3, 2, "autocommit")

    def test_timing_mapping_requires_exact_complete_output(self):
        text = f"{RUNNER.BEGIN}\nTiming is on\n{RUNNER.COMMAND}1\nRows affected: 1\nTime: 1.250 ms\nTiming is off\n{RUNNER.END}\n"
        self.assertEqual(RUNNER.execution_samples(text, ["insert"])[0]["cli_execute_ms"], 1.25)
        for bad in [text.replace(RUNNER.END, ""), text.replace("1.250", "NaN"),
                    text.replace("Rows affected: 1", "Error: refused"), text + RUNNER.END,
                    text.replace("Rows affected: 1", "Rows affected: 0"),
                    text.replace("1.250", "9" * 400),
                    text.replace(RUNNER.COMMAND + "1", RUNNER.COMMAND + "2")]:
            with self.subTest(text=bad), self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.execution_samples(bad, ["insert"])
        with self.assertRaises(RUNNER.BenchmarkError):
            RUNNER.execution_samples(text, ["insert", "commit"])

    def test_commit_without_cli_timer_is_not_fabricated_as_zero(self):
        text = f"{RUNNER.BEGIN}\n{RUNNER.COMMAND}1\n{RUNNER.COMMIT_ACK}\n{RUNNER.END}\n"
        sample = RUNNER.execution_samples(text, ["commit"])[0]
        self.assertIsNone(sample["cli_execute_ms"])
        self.assertEqual(sample["timing_status"], "unavailable_embedded_cli")
        for bad in [text.replace(RUNNER.COMMIT_ACK, ""),
                    text.replace(RUNNER.COMMIT_ACK, RUNNER.COMMIT_ACK + "\n" + RUNNER.COMMIT_ACK)]:
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.execution_samples(bad, ["commit"])

    def test_reopen_checks_values_not_only_row_count(self):
        self.assertEqual(len(RUNNER.verify_rows("1|3\n2|6\n", 2)), 64)
        for text in ["1|3\n2|7\n", "1|3\n", "2\n", "1|3\n1|3\n"]:
            with self.subTest(text=text), self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.verify_rows(text, 2)

    def test_statistics_refuse_missing_and_nonfinite_values(self):
        self.assertEqual(RUNNER.statistics([1, 2, 3])["p95"], 3)
        for values in [[], [math.nan], [math.inf], [-1]]:
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.statistics(values)

    def test_provenance_refuses_stale_or_mismatched_builds(self):
        # Synthetic fixture: this tests admission, not actual build success.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(source_root=root, build_dir=root,
                                   build_receipt=root / "receipt.json",
                                   sb_isql=root / "cli", database_seed=root / "seed",
                                   historical=True)
            args.sb_isql.write_bytes(b"test-only-cli")
            args.database_seed.write_bytes(b"test-only-seed")
            cache = root / "CMakeCache.txt"
            cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n" + "\n".join(
                f"{name}:BOOL=OFF" for name in (
                    "SCRATCHBIRD_ENABLE_DEBUG_LOGS", "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
                    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE", "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
                    "SB_ENABLE_TEST_CRASH_INJECTION", "SB_ENABLE_TEST_DML_ROUTE_SELECTION"))
                + f"\nCMAKE_HOME_DIRECTORY:INTERNAL={root / 'project'}\n")
            receipt = {"build_exit_code": 0, "correctness_exit_code": 0, "historical": True,
                       "source_commit": "fixture-revision",
                       "binary_sha256": {"sb_isql": RUNNER.sha256(args.sb_isql),
                                         "database_seed": RUNNER.sha256(args.database_seed)},
                       "cmake_cache_sha256": RUNNER.sha256(cache),
                       "tracked_patch_sha256": hashlib.sha256(b"").hexdigest()}

            def git_output(command):
                return b"fixture-revision\n" if "rev-parse" in command else b""

            with patch.object(RUNNER.subprocess, "check_output", side_effect=git_output), \
                    patch.object(RUNNER.platform, "platform", return_value="synthetic-platform"):
                args.build_receipt.write_text(json.dumps(receipt))
                self.assertTrue(RUNNER.provenance(args)["historical"])
                clean_cache = cache.read_text()
                for name in ("SB_ENABLE_TEST_CRASH_INJECTION", "SB_ENABLE_TEST_DML_ROUTE_SELECTION"):
                    off = f"{name}:BOOL=OFF"
                    for changed in (clean_cache.replace(off, f"{name}:BOOL=ON"),
                                    clean_cache.replace(off, ""),
                                    clean_cache + f"{name}:BOOL=ON\n"):
                        with self.subTest(flag=name, cache=changed):
                            cache.write_text(changed)
                            with self.assertRaisesRegex(RUNNER.BenchmarkError, "benchmark-clean"):
                                RUNNER.provenance(args)
                cache.write_text(clean_cache)
                for field, value in [("build_exit_code", 1), ("build_exit_code", False),
                                     ("correctness_exit_code", None), ("correctness_exit_code", 1),
                                     ("correctness_exit_code", False), ("historical", False),
                                     ("historical", None), ("source_commit", "different"),
                                     ("binary_sha256", {}), ("cmake_cache_sha256", "wrong"),
                                     ("tracked_patch_sha256", "wrong")]:
                    with self.subTest(field=field):
                        args.build_receipt.write_text(json.dumps({**receipt, field: value}))
                        with self.assertRaises(RUNNER.BenchmarkError):
                            RUNNER.provenance(args)
                args.build_receipt.write_text("[]")
                with self.assertRaises(RUNNER.BenchmarkError):
                    RUNNER.provenance(args)
                args.build_receipt.write_text(json.dumps(receipt))
                cache.write_text(cache.read_text().replace(str(root / "project"), "/wrong/clone/project"))
                with self.assertRaises(RUNNER.BenchmarkError):
                    RUNNER.provenance(args)

    def test_inherited_faults_refused_and_trace_selectors_not_propagated(self):
        with patch.dict(RUNNER.os.environ, {"PATH": "/example", "SB_API_TRACE_FILE": "trace"}, clear=True):
            self.assertEqual(RUNNER.clean_environment(), {"PATH": "/example"})
        for name in ("SCRATCHBIRD_FAULT_ENABLE", "SCRATCHBIRD_TEST_DML_OPTIMIZATION",
                     "SCRATCHBIRD_TEST_DML_ROUTE", "SB_TEST_OVERRIDE", "SB_TEST_TRACE_FILE"):
            with self.subTest(selector=name), patch.dict(RUNNER.os.environ, {name: "1"}, clear=True):
                with self.assertRaises(RUNNER.BenchmarkError):
                    RUNNER.clean_environment()

    def test_diagnostic_traces_require_both_complete_families(self):
        valid = "operation_id=persist_local_transaction_inventory\tok=true\ttotal_us=7\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = [root / filename for filename in RUNNER.TRACE_ENV.values()]
            paths[0].write_text(valid.replace("persist_local_transaction_inventory", "transaction.commit"))
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.collect_traces(root, 1)
            self.assertFalse((root / "phase-summary.json").exists())
            for malformed in ["", valid.rstrip("\n"), "not a timing\n"]:
                paths[1].write_text(malformed)
                with self.assertRaises(RUNNER.BenchmarkError):
                    RUNNER.collect_traces(root, 1)
            paths[1].write_text(valid)
            reports = RUNNER.collect_traces(root, 1)
            self.assertEqual(len(reports), 2)
            self.assertTrue(all(report["record_count"] == 1 for report in reports))
            self.assertTrue(all(not report["interpretation"]["nested_layers_additive"] for report in reports))
            with self.assertRaises(FileExistsError):
                RUNNER.collect_traces(root, 1)

    def test_failed_trace_operation_is_preserved_but_not_a_successful_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for filename in RUNNER.TRACE_ENV.values():
                (root / filename).write_text("operation_id=fixture.only\tok=false\ttotal_us=5\n")
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.collect_traces(root, 1)
            evidence = json.loads((root / "phase-summary.json").read_text())
            self.assertFalse(evidence["traces"][0]["operations"][0]["ok"])

    def test_unrelated_successful_traces_do_not_qualify_requested_commit_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for filename in RUNNER.TRACE_ENV.values():
                (root / filename).write_text("operation_id=unrelated\tok=true\ttotal_us=5\n")
            with self.assertRaises(RUNNER.BenchmarkError):
                RUNNER.collect_traces(root, 1)

    def test_real_combined_autocommit_trace_name_and_count(self):
        for operation, minimum, accepted in [
                ("transaction.autocommit_commit_and_begin", 1, True),
                ("transaction.autocommit_commit_and_begin", 2, False),
                ("transaction.autocommit_rollback_and_begin", 1, False)]:
            with self.subTest(operation=operation, minimum=minimum), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "transaction.tsv").write_text(f"operation_id={operation}\tok=true\ttotal_us=5\n")
                (root / "inventory.tsv").write_text(
                    "operation_id=persist_local_transaction_inventory\tok=true\ttotal_us=3\n")
                if accepted:
                    self.assertEqual(len(RUNNER.collect_traces(root, minimum)), 2)
                else:
                    with self.assertRaises(RUNNER.BenchmarkError):
                        RUNNER.collect_traces(root, minimum)


if __name__ == "__main__":
    unittest.main()
