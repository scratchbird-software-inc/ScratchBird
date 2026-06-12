#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate STATUS_CHANGE_AUTHORITY_MATRIX.csv for the SBsql Surface-to-SBLR fixtures.

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/STATUS_CHANGE_AUTHORITY_MATRIX.csv

Enumerates every legal canonical status transition and the lockstep
artifact updates that must accompany the transition. The status-change
authority gate (`sbsql_status_change_authority_gate.py`) reads this
matrix to know which paired updates to verify whenever a row's canonical
status changes.

Transitions covered:

  - native_future -> native_now (promote)
  - native_future -> cluster_private (private_profile_gate)
  - native_future -> removed (remove_by_spec_change)
  - native_now -> cluster_private (demote to cluster-only)
  - native_now -> removed (remove_by_spec_change)
  - cluster_private -> native_now (de-private; rare)
  - cluster_private -> native_future (rare)
  - cluster_private -> removed (remove_by_spec_change)
  - (any) -> same (no_change, no paired updates required)

Architecture invariant compliance: read-only CSV generation; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. MGA copy-on-write
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
OUTPUT_NAME = "STATUS_CHANGE_AUTHORITY_MATRIX.csv"
PUBLIC_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"


COLUMNS = [
    "transition_id",
    "from_status",
    "to_status",
    "transition_class",
    "required_canonical_updates",
    "required_execution_plan_artifact_updates",
    "required_test_and_inventory_updates",
    "decision_authority",
    "notes",
]


CANONICAL_UPDATES_PROMOTE = ";".join([
    "public_input_snapshot",
    "public_input_snapshot",
    "public_input_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
])
CANONICAL_UPDATES_DEMOTE_TO_CLUSTER = ";".join([
    "public_input_snapshot",
    "public_input_snapshot",
    "public_input_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
])
CANONICAL_UPDATES_REMOVE = ";".join([
    "public_input_snapshot",
    "public_input_snapshot",
    "public_input_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
])

EXECUTION_PLAN_ARTIFACT_UPDATES = ";".join([
    "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest",
    "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp",
    "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp",
    f"{PUBLIC_ARTIFACT_ROOT}/TARGET_ROW_COUNTS.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/STRICT_ROW_COVERAGE_LEDGER.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/FUNCTION_SEMANTIC_ORACLE_MATRIX.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/NATIVE_FUTURE_PROMOTION_MATRIX.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/PER_ROW_EVIDENCE_MANIFEST.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/SBSQL_SURFACE_RELEASE_DECLARATION.csv",
    f"{PUBLIC_ARTIFACT_ROOT}/SBSQL_SURFACE_RELEASE_DECLARATION.json",
])

TEST_INVENTORY_UPDATES = ";".join([
    "project/tests/sbsql_parser_worker/generated/full_surface_fixture_path_per_status",
    "project/tests/sbsql_parser_worker/CMakeLists.txt_when_new_ctest_label_required",
    "public_audit_summary<date>.md_public_inventory_re-rating",
])


TRANSITIONS = [
    {
        "transition_id": "NF_TO_NN_PROMOTE",
        "from_status": "native_future",
        "to_status": "native_now",
        "transition_class": "promote",
        "required_canonical_updates": CANONICAL_UPDATES_PROMOTE,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_SBSFC-003_promotion_matrix_decision_and_canonical_oracle_authority",
        "notes": "Promotion path. Requires canonical builtin-expression-registry entry before status change so that downstream slices can author row-level fixtures and reach e2e_passed.",
    },
    {
        "transition_id": "NF_TO_CP_PRIVATE_GATE",
        "from_status": "native_future",
        "to_status": "cluster_private",
        "transition_class": "private_profile_gate",
        "required_canonical_updates": CANONICAL_UPDATES_DEMOTE_TO_CLUSTER,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_SBSFC-003_decision_private_profile_gate",
        "notes": "Route native_future row to cluster_private treatment. Public build must fail-closed; private-profile evidence lives outside the public matrix per SBSFC-025.",
    },
    {
        "transition_id": "NF_TO_REMOVED",
        "from_status": "native_future",
        "to_status": "removed",
        "transition_class": "remove_by_spec_change",
        "required_canonical_updates": CANONICAL_UPDATES_REMOVE,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_SBSFC-003_decision_remove_by_spec_change",
        "notes": "Remove native_future row by canonical spec change. Row count decreases by 1 in every count surface; downstream slices must verify no orphaned references remain.",
    },
    {
        "transition_id": "NN_TO_CP_DEMOTE",
        "from_status": "native_now",
        "to_status": "cluster_private",
        "transition_class": "demote_to_cluster",
        "required_canonical_updates": CANONICAL_UPDATES_DEMOTE_TO_CLUSTER,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_explicit_demotion_justification_and_e2e_fixture_removal_plan",
        "notes": "Demote native_now row to cluster_private. Existing e2e_passed CTest fixtures must be removed or routed through the SBSFC-025 public-fail-closed gate; security-sensitive transition.",
    },
    {
        "transition_id": "NN_TO_REMOVED",
        "from_status": "native_now",
        "to_status": "removed",
        "transition_class": "remove_by_spec_change",
        "required_canonical_updates": CANONICAL_UPDATES_REMOVE,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_explicit_removal_justification_and_reference_alias_audit",
        "notes": "Remove native_now row by canonical spec change. Reference aliases mapping to this surface must be re-validated via SBSFC-041.",
    },
    {
        "transition_id": "CP_TO_NN_DEPRIVATE",
        "from_status": "cluster_private",
        "to_status": "native_now",
        "transition_class": "deprivate",
        "required_canonical_updates": CANONICAL_UPDATES_PROMOTE,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_explicit_deprivate_justification_and_canonical_oracle_authority",
        "notes": "Rare. Promote a previously cluster_private surface to public native_now. Requires full public oracle, fixture authoring, and public-fail-closed gate removal.",
    },
    {
        "transition_id": "CP_TO_NF",
        "from_status": "cluster_private",
        "to_status": "native_future",
        "transition_class": "park_as_future",
        "required_canonical_updates": CANONICAL_UPDATES_DEMOTE_TO_CLUSTER,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_explicit_parking_justification",
        "notes": "Rare. Move cluster_private row to native_future to defer promotion. Public refusal diagnostic class changes from CLUSTER_AUTHORITY_REQUIRED to NATIVE_FUTURE.",
    },
    {
        "transition_id": "CP_TO_REMOVED",
        "from_status": "cluster_private",
        "to_status": "removed",
        "transition_class": "remove_by_spec_change",
        "required_canonical_updates": CANONICAL_UPDATES_REMOVE,
        "required_execution_plan_artifact_updates": EXECUTION_PLAN_ARTIFACT_UPDATES,
        "required_test_and_inventory_updates": TEST_INVENTORY_UPDATES,
        "decision_authority": "coordinator_with_explicit_removal_justification",
        "notes": "Remove cluster_private row by canonical spec change. Public-fail-closed gate removes the per-row refusal test.",
    },
    {
        "transition_id": "NO_CHANGE",
        "from_status": "any",
        "to_status": "any_same",
        "transition_class": "no_change",
        "required_canonical_updates": "none",
        "required_execution_plan_artifact_updates": "none",
        "required_test_and_inventory_updates": "none",
        "decision_authority": "not_applicable",
        "notes": "Default case. Row status unchanged. No paired updates required; alignment-only audit by the status-change authority gate.",
    },
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(TRANSITIONS)

    print(f"status_change_authority_matrix=generated rows={len(TRANSITIONS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
