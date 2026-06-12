#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SBSQL_SURFACE_RELEASE_DECLARATION artifacts.

The release declaration is the machine-readable closure summary for the
SBsql surface-to-SBLR execution_plan. It is intentionally evidence-derived: row
release state comes from PER_ROW_EVIDENCE_MANIFEST and STRICT_ROW_COVERAGE_LEDGER,
while authenticated-route and SBLR binary round-trip matrices are carried as
explicit residual risk until those fixture layers are promoted to executable
``e2e_passed`` evidence by their dedicated gates.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path


REGISTRY_CSV = "public_input_snapshot"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
PER_ROW_MANIFEST_NAME = "PER_ROW_EVIDENCE_MANIFEST.csv"
STRICT_LEDGER_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
AUTH_ROUTE_MATRIX_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
ROUND_TRIP_MATRIX_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
OUTPUT_CSV_NAME = "SBSQL_SURFACE_RELEASE_DECLARATION.csv"
OUTPUT_JSON_NAME = "SBSQL_SURFACE_RELEASE_DECLARATION.json"


COLUMNS = [
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "final_status",
    "release_claim",
    "implementation_refs",
    "ctest_labels",
    "fixture_refs",
    "diagnostic_refs",
    "result_refs",
    "auth_route_ref",
    "sblr_round_trip_ref",
    "catalog_seed_ref",
    "reference_alias_ref",
    "remaining_risk",
    "release_status",
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_by_surface(rows: list[dict[str, str]], label: str) -> dict[str, dict[str, str]]:
    indexed: dict[str, dict[str, str]] = {}
    for row in rows:
        surface_id = row.get("surface_id", "")
        if not surface_id:
            fail(f"{label} contains a row without surface_id")
        if surface_id in indexed:
            fail(f"{label} contains duplicate surface_id {surface_id}")
        indexed[surface_id] = row
    return indexed


def release_claim_for(final_status: str, cluster_scope: str) -> str:
    if final_status == "e2e_passed":
        return "public_sbsql_e2e_implemented"
    if final_status == "cluster_provider_route_passed" and cluster_scope == "cluster_private":
        return "cluster_public_fail_closed_provider_gated"
    if final_status == "exact_refusal_passed" and cluster_scope == "cluster_private":
        return "cluster_public_fail_closed_provider_gated"
    if final_status == "exact_refusal_passed":
        return "public_exact_refusal_implemented"
    return "not_releasable"


def release_status_for(final_status: str) -> str:
    if final_status in {"e2e_passed", "exact_refusal_passed", "cluster_provider_route_passed"}:
        return "row_evidence_complete"
    return "blocked"


def strict_state_for(final_status: str) -> str:
    if final_status == "cluster_provider_route_passed":
        return "exact_refusal_passed"
    return final_status


def strict_state_compatible(final_status: str, strict_state: str) -> bool:
    if final_status in {"e2e_passed", "exact_refusal_passed", "cluster_provider_route_passed"}:
        return strict_state_for(final_status) == strict_state
    if final_status == "pending":
        return strict_state not in {
            "e2e_passed",
            "exact_refusal_passed",
            "cluster_provider_route_passed",
        }
    return final_status == strict_state


def risk_for(auth_row: dict[str, str], round_row: dict[str, str]) -> str:
    risks: list[str] = []
    auth_status = auth_row.get("fixture_status", "")
    round_status = round_row.get("fixture_status", "")
    if auth_status and auth_status != "e2e_passed":
        risks.append(f"authenticated_route_fixture_status={auth_status}")
    if round_status and round_status != "e2e_passed":
        risks.append(f"sblr_round_trip_fixture_status={round_status}")
    round_required = round_row.get("byte_identical_round_trip_required", "")
    if round_required and round_required.startswith("blocked_"):
        risks.append(f"round_trip_requirement={round_required}")
    if round_required == "yes_descriptor_oracle_pending":
        risks.append("descriptor_oracle_pending")
    return ";".join(risks) if risks else "none"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    registry = read_csv(root / REGISTRY_CSV)
    per_row = index_by_surface(read_csv(artifact_root / PER_ROW_MANIFEST_NAME), "PER_ROW_EVIDENCE_MANIFEST")
    strict = index_by_surface(read_csv(artifact_root / STRICT_LEDGER_NAME), "STRICT_ROW_COVERAGE_LEDGER")
    auth = index_by_surface(read_csv(artifact_root / AUTH_ROUTE_MATRIX_NAME), "AUTHENTICATED_FULL_ROUTE_MATRIX")
    round_trip = index_by_surface(read_csv(artifact_root / ROUND_TRIP_MATRIX_NAME), "SBLR_BINARY_ROUND_TRIP_MATRIX")

    if not registry:
        fail("SBSQL_SURFACE_REGISTRY.csv is empty")

    output_rows: list[dict[str, str]] = []
    final_counts: Counter[str] = Counter()
    release_counts: Counter[str] = Counter()
    risk_counts: Counter[str] = Counter()

    for surface in sorted(registry, key=lambda row: row["surface_id"]):
        surface_id = surface["surface_id"]
        per = per_row.get(surface_id)
        strict_row = strict.get(surface_id)
        auth_row = auth.get(surface_id)
        round_row = round_trip.get(surface_id)
        missing = [
            label
            for label, row in (
                ("per_row", per),
                ("strict", strict_row),
                ("auth_route", auth_row),
                ("round_trip", round_row),
            )
            if row is None
        ]
        if missing:
            fail(f"{surface_id} missing release declaration inputs: {','.join(missing)}")

        final_status = per["final_state"]
        if not strict_state_compatible(final_status, strict_row["current_state"]):
            fail(
                f"{surface_id} final-state drift: per_row={final_status} strict={strict_row['current_state']}"
            )

        claim = release_claim_for(final_status, surface["cluster_scope"])
        release_status = release_status_for(final_status)
        risk = risk_for(auth_row, round_row)

        output_rows.append(
            {
                "surface_id": surface_id,
                "fixed_uuid_v7": surface["fixed_uuid_v7"],
                "canonical_name": surface["canonical_name"],
                "surface_kind": surface["surface_kind"],
                "family": surface["family"],
                "final_status": final_status,
                "release_claim": claim,
                "implementation_refs": per["implementation_refs"],
                "ctest_labels": per["ctest_label"],
                "fixture_refs": per["fixture_path"],
                "diagnostic_refs": per["diagnostic_proof"],
                "result_refs": per["result_proof"],
                "auth_route_ref": f"{auth_row['fixture_path']}#{auth_row['fixture_status']}",
                "sblr_round_trip_ref": f"{round_row['fixture_path']}#{round_row['fixture_status']}",
                "catalog_seed_ref": "function_seed_registry_or_catalog_seed_gate_when_applicable",
                "reference_alias_ref": "reference_alias_matrix_when_applicable",
                "remaining_risk": risk,
                "release_status": release_status,
            }
        )
        final_counts[final_status] += 1
        release_counts[release_status] += 1
        risk_counts[risk] += 1

    blocked_rows = sum(1 for row in output_rows if row["release_status"] == "blocked")
    residual_risk_rows = sum(1 for row in output_rows if row["remaining_risk"] != "none")
    auth_pending_rows = sum(
        1 for row in output_rows if "authenticated_route_fixture_status=pending_authoring" in row["remaining_risk"]
    )
    round_trip_pending_rows = sum(
        1 for row in output_rows if "sblr_round_trip_fixture_status=pending_authoring" in row["remaining_risk"]
    )

    output_csv = artifact_root / OUTPUT_CSV_NAME
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(output_rows)

    if blocked_rows:
        release_claim = "blocked_pending_row_evidence"
        notes = (
            "Release declaration is generated honestly with blocked rows. Row-level SBsql implementation "
            "evidence is not complete until pending rows are implemented or explicitly refused with final proof."
        )
    elif residual_risk_rows:
        release_claim = "row_evidence_complete_route_fixture_layers_pending"
        notes = (
            "Row-level SBsql implementation evidence is complete. Authenticated-route and SBLR binary round-trip "
            "fixture matrices remain explicit residual risk until their fixture_status values are promoted to "
            "e2e_passed by dedicated executable route and binary round-trip gates."
        )
    else:
        release_claim = "release_ready"
        notes = "Row-level SBsql implementation evidence is complete. Authenticated-route and SBLR binary round-trip fixture layers are promoted to e2e_passed evidence."

    summary = {
        "search_key": "SBSQL-SURFACE-SBLR-RELEASE-DECLARATION",
        "status": "blocked" if blocked_rows else "row_evidence_complete",
        "generated_from": [
            "artifacts/PER_ROW_EVIDENCE_MANIFEST.csv",
            "artifacts/STRICT_ROW_COVERAGE_LEDGER.csv",
            "artifacts/AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
            "artifacts/SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
            "public_input_snapshot",
        ],
        "expected_total_rows": len(registry),
        "declared_rows": len(output_rows),
        "implemented_rows": final_counts["e2e_passed"],
        "exact_refused_rows": final_counts["exact_refusal_passed"],
        "cluster_provider_route_rows": final_counts["cluster_provider_route_passed"],
        "private_gated_rows": sum(1 for row in output_rows if row["release_claim"] == "cluster_public_fail_closed_provider_gated"),
        "blocked_rows": blocked_rows,
        "release_status_counts": dict(sorted(release_counts.items())),
        "final_status_counts": dict(sorted(final_counts.items())),
        "remaining_risk_rows": residual_risk_rows,
        "authenticated_route_pending_rows": auth_pending_rows,
        "sblr_round_trip_pending_rows": round_trip_pending_rows,
        "top_remaining_risks": dict(risk_counts.most_common(10)),
        "release_claim": release_claim,
        "notes": notes,
    }

    output_json = artifact_root / OUTPUT_JSON_NAME
    output_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(
        "sbsql_surface_release_declaration=generated "
        f"rows={len(output_rows)} "
        + " ".join(f"{k}={v}" for k, v in sorted(final_counts.items()))
        + f" auth_pending={auth_pending_rows} round_trip_pending={round_trip_pending_rows}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
