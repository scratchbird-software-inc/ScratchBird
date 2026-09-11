#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Harness checks only; no simulated engine results count as storage proof."""
import unittest
import subprocess
from unittest.mock import Mock, call, patch
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace

import sbsql_copy_persistence_full_route_gate as route_support
from sbsql_copy_persistence_full_route_gate import IsqlResult
from sbsql_insert_route_equivalence_gate import (
    histories, observation, require_route_evidence, mutation_inventory,
    require_optimization_evidence, mixed_history, failure_class,
    require_mixed_optimization_evidence,
)
from dml_history_reducer import reduce_schedule
from dml_snapshot_equivalence_gate import run as run_engine_probe


class InsertEquivalenceHarnessTest(unittest.TestCase):
    def test_mixed_routes_require_real_candidate_branches(self):
        optimized = "delete_canonical_scan\nupdate_index_candidates\nhot_point_cache_miss\n"
        scalar = "delete_canonical_scan\nupdate_table_scan\nhot_point_cache_disabled\n"
        require_mixed_optimization_evidence(optimized, "optimized")
        require_mixed_optimization_evidence(scalar, "scan_scalar")
        for text, profile in ((scalar, "optimized"), (optimized, "scan_scalar"), ("", "staged")):
            with self.assertRaises(RuntimeError):
                require_mixed_optimization_evidence(text, profile)

    def test_engine_probe_timeout_preserves_partial_schedule(self):
        with TemporaryDirectory(prefix="sbi9_unit_") as directory:
            root = Path(directory)
            timeout = subprocess.TimeoutExpired(["probe"], 180, output=b"action\t1\n", stderr=b"partial\n")
            with patch("dml_snapshot_equivalence_gate.subprocess.run", side_effect=timeout):
                with self.assertRaises(subprocess.TimeoutExpired):
                    run_engine_probe(["probe"], root, "history", {})
            self.assertEqual((root / "history.stdout").read_text(), "action\t1\n")
            self.assertEqual((root / "history.stderr").read_text(), "partial\n")
            self.assertTrue((root / "history.command.json").exists())

    def test_reducer_retains_epochs_and_reproducible_failure_class(self):
        original = [[{"value": 1}, {"value": 2}], [{"value": 3}], [{"value": 4}]]
        def fails(schedule, attempt):
            return any(action["value"] == 3 for epoch in schedule for action in epoch)
        reduced, attempts = reduce_schedule(original, fails)
        self.assertEqual(reduced, [[], [{"value": 3}], []])
        self.assertLessEqual(attempts, 32)
        self.assertEqual(original[0], [{"value": 1}, {"value": 2}])
        self.assertEqual(reduce_schedule(original, fails, 0), (original, 0))
        self.assertIsNone(failure_class(TimeoutError("transport timed out")))

    def test_reduced_model_recomputes_expected_rows_and_finality(self):
        original = mixed_history(901)
        self.assertEqual(mixed_history(901, original.schedule), original)
        reduced = mixed_history(901, [[], [], []])
        self.assertEqual(reduced.final_rows, [(6, "stream-baseline"),
                         (20000, "anchor-0"), (20002, "anchor-2")])
        self.assertEqual(reduced.transaction_states, ("committed", "rolled_back", "committed"))
        self.assertEqual(reduced.execution_rows, (1, 1, 1))

    def test_optimization_evidence_requires_the_actual_fallback(self):
        require_optimization_evidence("cache_evicted_before_proof\ncache_loss_scoped_reload\n", "evicted")
        require_optimization_evidence("load_indexes\n", "uncached")
        with self.assertRaises(RuntimeError):
            require_optimization_evidence("cache_evicted_before_proof\n", "evicted")
        with self.assertRaises(RuntimeError):
            require_optimization_evidence("load_indexes\ncache_publish\n", "uncached")

    def test_seed_reproducibility(self):
        self.assertEqual(histories(901), histories(901))
        self.assertNotEqual(histories(901), histories(902))
        self.assertEqual(len(histories(901)), 13)

    def test_baseline_is_retained_and_final_keys_are_unique(self):
        for seed in (0, 901, 902, 2**63):
            for case in histories(seed):
                self.assertIn((6, "stream-baseline"), case.final_rows)
                self.assertEqual(len(case.final_rows), len(dict(case.final_rows)))
                if case.admitted:
                    self.assertGreater(case.inserted_rows, 0)
                else:
                    self.assertEqual(case.diagnostics, ("SBLR.OPERATION.NONCANONICAL",))
                self.assertEqual(sum(case.execution_rows), case.inserted_rows)
                if not case.execution_rows:
                    self.assertEqual(case.transaction_states, ())
                self.assertTrue(case.sql.endswith("\n"))

    def test_counts_and_error_text_are_not_normalized(self):
        first = IsqlResult("fixture", 0, "Rows affected: 1", "")
        second = IsqlResult("fixture", 0, "Rows affected: 2", "")
        self.assertNotEqual(observation(first), observation(second))
        error = IsqlResult("fixture", 1, "", "Error: detail A (EXPECTED)")
        changed = IsqlResult("fixture", 1, "", "Error: detail B (EXPECTED)")
        self.assertNotEqual(observation(error, ("EXPECTED",)),
                            observation(changed, ("EXPECTED",)))

    def test_unexpected_refusal_never_counts_as_equivalence(self):
        result = IsqlResult("fixture", 1, "", "Error: detail (UNSUPPORTED)")
        with self.assertRaises(RuntimeError):
            observation(result)
        with self.assertRaises(RuntimeError):
            observation(result, ("EXPECTED",))

    def test_diagnostic_detail_is_validated_without_erasing_it(self):
        first = IsqlResult("fixture", 1, "", "Error: refusal (EXPECTED;detail:first)")
        second = IsqlResult("fixture", 1, "", "Error: refusal (EXPECTED;detail:second)")
        self.assertNotEqual(observation(first, ("EXPECTED",)),
                            observation(second, ("EXPECTED",)))
        for error in ("Error: refusal (UNEXPECTED;EXPECTED)",
                      "Error: refusal (EXPECTED_EXTRA;detail)",
                      "Error: refusal (EXPECTED;detail"):
            with self.assertRaises(RuntimeError):
                observation(IsqlResult("fixture", 1, "", error), ("EXPECTED",))

    def test_missing_or_extra_errors_are_refused(self):
        for result in (IsqlResult("fixture", 0, "", ""),
                       IsqlResult("fixture", 1, "", ""),
                       IsqlResult("fixture", 1, "", "Error: x (EXPECTED)\nextra")):
            with self.assertRaises(RuntimeError):
                observation(result, ("EXPECTED",))

    def test_startup_failure_stops_listener_and_server(self):
        with TemporaryDirectory(prefix="sbi9_unit_") as directory:
            root = Path(directory)
            database = root / "existing.sbdb"
            database.touch()
            args = SimpleNamespace(server="unused", listener="unused",
                                   parser_worker="unused")
            server, listener = Mock(), Mock()
            with patch.object(route_support, "find_free_port", return_value=1), \
                 patch.object(route_support, "wait_for_path"), \
                 patch.object(route_support, "wait_for_tcp", side_effect=RuntimeError("startup")), \
                 patch.object(route_support.subprocess, "Popen", side_effect=[server, listener]) as popen, \
                 patch.object(route_support, "stop_process") as stopped:
                with self.assertRaisesRegex(RuntimeError, "startup"):
                    route_support.start_route(args, root / "r", database, tls_required=False)
                self.assertEqual(stopped.call_args_list, [call(listener), call(server)])
                # Mock Popen retains kwargs; release the redirected files on
                # Windows too before TemporaryDirectory removes the fixture.
                for started in popen.call_args_list:
                    started.kwargs["stdout"].close()
                    started.kwargs["stderr"].close()

    def test_route_evidence_requires_actual_executor_and_all_inserted_rows(self):
        self.assertEqual(require_route_evidence("direct\t2\t10\n", "optimized", 2), [10])
        self.assertEqual(require_route_evidence("staged\t1\t10\nstaged\t1\t10\n", "staged", 2), [10])
        for text in ("", "direct\t2\t10\n", "staged\t-2\t10\n", "staged\ttwo\t10\n",
                     "staged\t1\t10\n", "staged\t2\textra\n", "staged\t2\t0\n"):
            with self.assertRaises(RuntimeError):
                require_route_evidence(text, "staged", 2)

    def test_refusal_is_not_executor_coverage(self):
        self.assertEqual(require_route_evidence("", "optimized", 0), [])
        with self.assertRaises(RuntimeError):
            require_route_evidence("direct\t0\t10\n", "optimized", 0)

    def test_surrounding_work_cannot_hide_refused_statement_execution(self):
        expected = (1, 1)
        self.assertEqual(require_route_evidence(
            "direct\t1\t10\ndirect\t1\t10\n", "optimized", 2, expected), [10])
        for text in ("direct\t1\t10\ndirect\t0\t10\ndirect\t1\t10\n",
                     "direct\t2\t10\n"):
            with self.assertRaisesRegex(RuntimeError, "executor sequence"):
                require_route_evidence(text, "optimized", 2, expected)

    def test_inventory_correspondence_preserves_actual_finality(self):
        first = "next_local_transaction_id\t14\n10\trolled_back\t0\t9\t1\t1\t0\n13\tcommitted\t0\t12\t1\t1\t0\n"
        second = "next_local_transaction_id\t16\n11\trolled_back\t0\t10\t1\t1\t0\n15\tcommitted\t0\t10\t1\t1\t0\n"
        expected = ("rolled_back", "committed")
        self.assertEqual(mutation_inventory(first, [10, 13], expected),
                         mutation_inventory(second, [11, 15], expected))
        for broken in (first.replace("rolled_back", "committed"),
                       first.replace("\t1\t1\t0", "\t1\t0\t0")):
            with self.assertRaises(RuntimeError):
                mutation_inventory(broken, [10, 13], expected)
        with self.assertRaises(RuntimeError):
            mutation_inventory(first, [10], expected)


if __name__ == "__main__":
    unittest.main()
