#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-030 index readiness manifest generation.

SEARCH_KEY: CEIC_030_INDEX_READINESS_MANIFEST_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile


REQUIRED_FAMILY_FIELDS = (
    "family_id",
    "family_name",
    "provider_classification",
    "persistent",
    "runtime_availability",
    "route_capability_summary",
    "family_classification_summary",
    "storage_authority_status",
    "mga_cow_recovery_proof_status",
    "security_exact_recheck_proof_status",
    "metric_producer_status",
    "benchmark_evidence_status",
    "scale_evidence_status",
    "crash_evidence_status",
    "corruption_evidence_status",
    "cleanup_evidence_status",
    "readiness_drift_gate_status",
    "policy_reference_cluster_claim_boundary",
    "auditor_timestamp_utc",
    "generation_metadata",
    "enterprise_ready",
)


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if expect_success and result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected success: {' '.join(command)}")
    if not expect_success and result.returncode == 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected failure: {' '.join(command)}")
    return result


def load(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: pathlib.Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def expected_registry_families(repo_root: pathlib.Path) -> list[str]:
    header = (repo_root / "project/src/core/index/index_family_registry.hpp").read_text(encoding="utf-8")
    match = re.search(r"enum class IndexFamily\s*:[^{]+{(?P<body>.*?)};", header, re.S)
    if not match:
        raise AssertionError("IndexFamily enum not found")
    names = []
    for raw in match.group("body").split(","):
        name = raw.strip().split("=", 1)[0].strip()
        if name and name != "unknown":
            names.append(name)
    return names


def assert_manifest_shape(repo_root: pathlib.Path, data: dict) -> None:
    if data.get("schema_version") != 1:
        raise AssertionError("manifest schema_version must be 1")
    if not data.get("generated_at_utc"):
        raise AssertionError("manifest generated_at_utc is required")
    if not data.get("auditor", {}).get("timestamp_utc"):
        raise AssertionError("manifest auditor timestamp is required")
    ceic_042_scope = data.get("readiness_policy", {}).get("ceic_042_scope")
    if ceic_042_scope not in {
        "future_ci_drift_gate_not_completed_by_CEIC_030",
        "ci_readiness_drift_gate_complete_for_index_manifest_only_no_all_index_enterprise_cluster_or_integrated_readiness_claim",
    }:
        raise AssertionError("CEIC-042 scope state is invalid")
    if data.get("readiness_policy", {}).get("ceic_038_scope") != "lossy_candidate_pruning_family_classification_only_no_provider_metrics_crash_drift_readiness_claim":
        raise AssertionError("CEIC-038 must remain classification-only scope")
    successor_rows = data.get("index_successor_slice_statuses")
    if not isinstance(successor_rows, list):
        raise AssertionError("index_successor_slice_statuses is required")
    successor = {row.get("slice_id"): row.get("status") for row in successor_rows if isinstance(row, dict)}
    if successor.get("CEIC-031") not in {"pending", "complete", "completed", "done", "closed", "complete_move_ready"}:
        raise AssertionError("CEIC-031 successor status must be tracked")
    if successor.get("CEIC-040") not in {"complete", "completed", "done", "closed", "complete_move_ready"}:
        raise AssertionError("CEIC-040 must be complete when runtime metric proof is present")
    if successor.get("CEIC-041") not in {"complete", "completed", "done", "closed", "complete_move_ready"}:
        raise AssertionError("CEIC-041 must be complete when crash/corruption matrix proof is present")
    if successor.get("CEIC-042") not in {"pending", "complete", "completed", "done", "closed", "complete_move_ready"}:
        raise AssertionError("CEIC-042 successor status must be tracked")
    expected_pending = [
        f"CEIC-{value:03d}" for value in range(31, 43)
        if successor.get(f"CEIC-{value:03d}") in {"pending", "planned"}
    ]
    if data.get("future_index_slices_pending") != expected_pending:
        raise AssertionError("future_index_slices_pending must track only currently pending successor slices")
    integrated = {
        row.get("slice_id"): row.get("status")
        for row in data.get("integrated_boundary_slice_statuses", [])
        if isinstance(row, dict)
    }
    for value in range(90, 96):
        if integrated.get(f"CEIC-{value:03d}") not in {"complete", "completed", "done", "closed", "complete_move_ready"}:
            raise AssertionError(f"CEIC-{value:03d} integrated proof must be complete")
    drift = data.get("readiness_drift_gate_evidence", {})
    if successor.get("CEIC-042") in {"complete", "completed", "done", "closed", "complete_move_ready"}:
        if drift.get("state") != "complete":
            raise AssertionError("CEIC-042 drift gate evidence must be complete")
        if drift.get("static_or_smoke_only") is not False:
            raise AssertionError("CEIC-042 must reject static/smoke-only evidence")
    elif drift.get("state") != "pending":
        raise AssertionError("CEIC-042 drift gate evidence must be pending until complete")

    families = data.get("families")
    if not isinstance(families, list):
        raise AssertionError("families must be a list")
    enum_names = [row.get("enum_name") for row in families]
    expected = expected_registry_families(repo_root)
    if enum_names != expected:
        raise AssertionError(f"manifest families do not match registry order: {enum_names} != {expected}")
    family_ids = [row.get("family_id") for row in families]
    if len(family_ids) != len(set(family_ids)):
        raise AssertionError("family ids must be unique")

    for row in families:
        missing = sorted(set(REQUIRED_FAMILY_FIELDS) - set(row))
        if missing:
            raise AssertionError(f"{row.get('family_id', '<unknown>')} missing fields {missing}")
        if row.get("enterprise_ready") is not False:
            raise AssertionError(f"{row.get('family_id')} must not be enterprise_ready in CEIC-030")
        route = row["route_capability_summary"]
        for field in ("supported_routes", "requires_exact_recheck", "requires_mga_recheck", "requires_security_recheck", "routes"):
            if field not in route:
                raise AssertionError(f"{row['family_id']} missing route field {field}")
        boundary = row["policy_reference_cluster_claim_boundary"]
        for field in (
            "transaction_finality_authority",
            "visibility_authority",
            "authorization_security_authority",
            "security_authority",
            "recovery_authority",
            "parser_authority",
            "reference_authority",
            "wal_authority",
            "benchmark_authority",
            "optimizer_plan_authority",
            "optimizer_plan_finality_authority",
            "index_finality_authority",
            "row_truth_authority",
            "final_row_authority",
            "result_finality_authority",
            "local_cluster_authority",
            "cluster_authority",
            "cluster_action_authority",
            "agent_action_authority",
        ):
            if boundary.get(field) is not False:
                raise AssertionError(f"{row['family_id']} {field} must be false")
        classification = row["family_classification_summary"]
        for field in (
            "row_truth_authority",
            "final_row_authority",
            "ceic_039_specialized_provider_closure_claimed",
            "ceic_040_runtime_metrics_claimed",
            "ceic_041_crash_matrix_claimed",
            "ceic_042_readiness_drift_claimed",
            "all_index_readiness_claimed",
            "reference_dominance_claimed",
            "enterprise_readiness_claimed",
        ):
            if classification.get(field) is not False:
                raise AssertionError(f"{row['family_id']} classification {field} must be false")
        if row.get("persistent") is True:
            for field in (
                "storage_authority_status",
                "mga_cow_recovery_proof_status",
                "security_exact_recheck_proof_status",
                "metric_producer_status",
                "benchmark_evidence_status",
                "scale_evidence_status",
                "crash_evidence_status",
                "corruption_evidence_status",
                "cleanup_evidence_status",
            ):
                allowed_statuses = {"pending", "blocked", "not_applicable"}
                if field == "security_exact_recheck_proof_status":
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if field == "metric_producer_status":
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if field in {
                    "storage_authority_status",
                    "mga_cow_recovery_proof_status",
                    "benchmark_evidence_status",
                    "crash_evidence_status",
                    "corruption_evidence_status",
                    "cleanup_evidence_status",
                    "readiness_drift_gate_status",
                }:
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if row[field]["status"] not in allowed_statuses:
                    raise AssertionError(f"{row['family_id']} {field} overclaims {row[field]['status']}")
            if row["metric_producer_status"]["status"] != "complete":
                raise AssertionError(f"{row['family_id']} CEIC-040 metric producer must be complete")
            if row["persistence_class"] not in {"reference_emulated", "policy_blocked"}:
                for field in (
                    "crash_evidence_status",
                    "corruption_evidence_status",
                    "cleanup_evidence_status",
                ):
                    if row[field]["status"] != "complete":
                        raise AssertionError(f"{row['family_id']} CEIC-041 {field} must be complete")
            if successor.get("CEIC-042") in {"complete", "completed", "done", "closed", "complete_move_ready"}:
                if row["benchmark_evidence_status"]["status"] != "complete":
                    raise AssertionError(f"{row['family_id']} CEIC-042 benchmark status must be complete")
                if row["readiness_drift_gate_status"]["status"] != "complete":
                    raise AssertionError(f"{row['family_id']} CEIC-042 drift gate status must be complete")

    by_enum = {row["enum_name"]: row for row in families}
    bloom = by_enum["bloom"]
    if bloom["family_classification_summary"]["semantic_class"] != "bloom_negative_prune":
        raise AssertionError("bloom semantic_class must be bloom_negative_prune")
    if bloom["family_classification_summary"]["bloom_negative_prune_only"] is not True:
        raise AssertionError("bloom must be negative-prune only")
    if bloom["route_capability_summary"]["produces_candidate_set"] is not False:
        raise AssertionError("bloom must not produce row candidate sets")

    for enum_name in ("brin_zone", "columnar_zone"):
        row = by_enum[enum_name]
        if row["family_classification_summary"]["summary_segment_prune_only"] is not True:
            raise AssertionError(f"{enum_name} must be summary/segment-prune only")
        if row["route_capability_summary"]["produces_candidate_set"] is not False:
            raise AssertionError(f"{enum_name} must not produce candidate sets directly")

    for enum_name in ("vector_hnsw", "vector_ivf", "sparse_wand"):
        row = by_enum[enum_name]
        if row["family_classification_summary"]["requires_exact_rerank"] is not True:
            raise AssertionError(f"{enum_name} must require exact rerank")
        if row["family_classification_summary"]["requires_exact_fallback"] is not True:
            raise AssertionError(f"{enum_name} must require exact fallback")

    hash_row = by_enum["hash"]
    if hash_row["family_classification_summary"]["hash_equality_only"] is not True:
        raise AssertionError("hash must remain equality-only")
    if hash_row["route_capability_summary"]["supports_ordered_range"] is not False:
        raise AssertionError("hash must not support ordered range")

    for enum_name, classification in (
        ("reference_emulated", "reference_emulated_mapping_non_authority"),
        ("policy_blocked", "policy_blocked_non_runtime"),
    ):
        row = by_enum[enum_name]
        if row["provider_classification"] != classification:
            raise AssertionError(f"{enum_name} classification drifted")
        if row["runtime_availability"]["runtime_available_static_input"] is not False:
            raise AssertionError(f"{enum_name} must remain non-runtime")
        if row["storage_authority_status"]["status"] != "blocked":
            raise AssertionError(f"{enum_name} storage authority must be blocked")


def expect_failure_contains(command: list[str], text: str) -> None:
    result = run(command, expect_success=False)
    output = result.stdout + result.stderr
    if text not in output:
        raise AssertionError(f"expected failure output to contain {text!r}, got: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_index_readiness_manifest.py"
    committed_manifest = (
        repo_root
        / "project/tests/release_evidence/consolidated_enterprise_public_evidence/artifacts/"
        / "CEIC-030_INDEX_READINESS_MANIFEST.yaml"
    )

    committed_result = run(
        [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(committed_manifest)],
        expect_success=True,
    )
    assert_manifest_shape(repo_root, load(committed_manifest))

    with tempfile.TemporaryDirectory(prefix="ceic_030_index_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        generated = temp_dir / "CEIC-030_INDEX_READINESS_MANIFEST.yaml"
        generated_result = run(
            [
                sys.executable,
                str(tool),
                "--repo-root",
                str(repo_root),
                "--manifest",
                str(generated),
                "--write",
            ],
            expect_success=True,
        )
        assert_manifest_shape(repo_root, load(generated))

        stale = load(generated)
        stale["source_evidence_digest"] = "0" * 64
        stale_path = temp_dir / "stale.yaml"
        write(stale_path, stale)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(stale_path)],
            "stale manifest differs",
        )

        reference_overclaim = load(generated)
        for row in reference_overclaim["families"]:
            if row["enum_name"] == "reference_emulated":
                row["storage_authority_status"]["status"] = "pending"
        reference_path = temp_dir / "reference_overclaim.yaml"
        write(reference_path, reference_overclaim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(reference_path)],
            "reference_emulated storage authority must be blocked",
        )

        persistent_overclaim = load(generated)
        for row in persistent_overclaim["families"]:
            if row["persistent"]:
                row["enterprise_ready"] = True
                break
        persistent_path = temp_dir / "persistent_overclaim.yaml"
        write(persistent_path, persistent_overclaim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(persistent_path)],
            "enterprise_ready must be false",
        )

        no_timestamp = load(generated)
        no_timestamp["generated_at_utc"] = ""
        no_timestamp_path = temp_dir / "no_timestamp.yaml"
        write(no_timestamp_path, no_timestamp)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(no_timestamp_path)],
            "generated_at_utc is required",
        )

    print("ceic_030_index_readiness_manifest_gate_test=pass")
    print(committed_result.stdout.strip())
    print(generated_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
