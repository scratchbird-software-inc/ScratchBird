#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Negative controls for the driver source-inventory guard (not engine tests)."""

import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

from source_coverage_contract import (
    AUTH_ROUTE_ROOT_REL, RELEASE_REL, REPLAY_INDEX_REL, SBLR_ROUND_TRIP_ROOT_REL,
    identity_digest, quoted_field, validate_replay_rows, validate_source_coverage,
)


class SourceCoverageTest(unittest.TestCase):
    def setUp(self):
        self.release = [{"surface_id": str(i), "final_status": status} for i, status in enumerate(
            ("e2e_passed", "exact_refusal_passed", "cluster_provider_route_passed"))]
        self.replay = [{"surface_id": row["surface_id"],
                        "expected_server_result": "release-evidence=" + row["final_status"],
                        "route_set": "full_route" if row["final_status"] == "e2e_passed" else "diagnostic"}
                       for row in self.release]
        self.contract = {"surface_identity_sha256": identity_digest({"0", "1", "2"}),
                         "disposition_counts": {row["final_status"]: 1 for row in self.release}}

    def test_partition(self):
        self.assertEqual(validate_replay_rows(self.release, self.replay, self.contract), [])

    def test_duplicate_identity_cannot_pad_count(self):
        with self.assertRaises(ValueError):
            validate_replay_rows(self.release, self.replay + [self.replay[0]], self.contract)

    def test_missing_identity(self):
        self.assertIn("replay:identity_set_mismatch",
                      validate_replay_rows(self.release, self.replay[:-1], self.contract))

    def test_same_size_identity_replacement(self):
        self.release[0]["surface_id"] = "replacement"
        self.replay[0]["surface_id"] = "replacement"
        self.assertIn("release:identity_digest_mismatch",
                      validate_replay_rows(self.release, self.replay, self.contract))

    def test_refusal_cannot_count_as_execution(self):
        self.replay[1]["route_set"] = "full_route"
        self.assertIn("replay:full_route_mismatch:1",
                      validate_replay_rows(self.release, self.replay, self.contract))

    def test_lost_full_route(self):
        self.replay[0]["route_set"] = "diagnostic"
        self.assertIn("replay:full_route_mismatch:0",
                      validate_replay_rows(self.release, self.replay, self.contract))

    def test_reclassification_requires_contract_change(self):
        self.release[0]["final_status"] = "exact_refusal_passed"
        self.assertIn("release:disposition_counts_mismatch",
                      validate_replay_rows(self.release, self.replay, self.contract))

    def test_conflicting_evidence_is_not_accepted(self):
        self.replay[0]["expected_server_result"] += ";release-evidence=exact_refusal_passed"
        self.assertIn("replay:disposition_mismatch:0",
                      validate_replay_rows(self.release, self.replay, self.contract))

    def test_flat_fields_reject_missing_and_duplicate(self):
        for text in ("", 'surface_id: "a"\nsurface_id: "b"\n'):
            with self.assertRaises(ValueError):
                quoted_field(text, "surface_id")
        self.assertEqual(quoted_field('surface_id: "a"\n', "surface_id"), "a")

    def test_query_replacement_is_mandatory_and_separate(self):
        with tempfile.TemporaryDirectory(prefix="sb-driver-source-contract-") as directory:
            root = Path(directory)
            self.release[0]["fixture_refs"] = "query_test.py"
            for row in self.release[1:]:
                row["fixture_refs"] = ""
            for relative, rows in ((RELEASE_REL, self.release), (REPLAY_INDEX_REL, self.replay)):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                    writer.writeheader()
                    writer.writerows(rows)
            (root / "query_test.py").write_text("# executable fixture location\n")
            for relative, suffix in ((AUTH_ROUTE_ROOT_REL, ".route.yaml"),
                                     (SBLR_ROUND_TRIP_ROOT_REL, ".round_trip.yaml")):
                (root / relative).mkdir(parents=True, exist_ok=True)
                for row in self.release:
                    fields = {"surface_id": row["surface_id"], "per_row_final_state": row["final_status"],
                              "per_row_fixture_path": "query_test.py", "implementation_refs": "query.execute;no_scalar"}
                    (root / relative / (row["surface_id"] + suffix)).write_text(
                        "".join(f"{key}: {json.dumps(value)}\n" for key, value in fields.items()))
            manifest = {"replay_source_contract": self.contract,
                        "required_query_fixture_contracts": [{"surface_id": "0", "fixture_path": "query_test.py",
                                                              "required_implementation_refs": ["query.execute", "no_scalar"]}],
                        "scalar_and_query_coverage_minimums": {"fixture_rows": 2, "fixture_surface_ids": 2}}
            self.assertEqual(validate_source_coverage(root, manifest, {"scalar"}, 1), [])
            self.assertIn("query_fixture:duplicate_or_scalar_marker:0",
                          validate_source_coverage(root, manifest, {"0"}, 1))
            changed = copy.deepcopy(manifest)
            changed["required_query_fixture_contracts"] = []
            self.assertIn("coverage:scalar_and_query_rows_below_minimum",
                          validate_source_coverage(root, changed, {"scalar"}, 1))
            changed = copy.deepcopy(manifest)
            changed["required_query_fixture_contracts"][0]["required_implementation_refs"].append("missing")
            self.assertIn("query_fixture:missing_route_contract:0",
                          validate_source_coverage(root, changed, {"scalar"}, 1))
            (root / "query_test.py").unlink()
            self.assertIn("query_fixture:missing_executable:0",
                          validate_source_coverage(root, manifest, {"scalar"}, 1))


if __name__ == "__main__":
    unittest.main()
