#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the CEIC-024 memory readiness manifest.

SEARCH_KEY: CEIC_024_MEMORY_READINESS_MANIFEST_TOOL
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys
import uuid
from dataclasses import dataclass
from typing import Any, Iterable


EXECUTION_PLAN = pathlib.Path("project/tests/release_evidence/consolidated_enterprise_public_evidence")
DEFAULT_MANIFEST = EXECUTION_PLAN / "artifacts/CEIC-024_MEMORY_READINESS_MANIFEST.yaml"

AUTHORITY_BOUNDARY_TOKEN = (
    "memory_evidence_only_not_transaction_finality_visibility_authorization_security_"
    "recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"
)

COMPLETE_STATUS = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUS = {"present", "complete", "completed", "generated"}

MEMORY_COMPLETED_SLICES = tuple(f"CEIC-{value:03d}" for value in range(10, 24))
MEMORY_MANIFEST_SLICES = MEMORY_COMPLETED_SLICES + ("CEIC-024",)
MEMORY_EXTENSION_SLICES = ("CEIC-025", "CEIC-026", "CEIC-027", "CEIC-028", "CEIC-029")
INTEGRATED_RELEASE_SLICES = ("CEIC-091", "CEIC-093", "CEIC-094")
INTEGRATED_RELEASE_ARTIFACTS = {
    "CEIC-091": ("CEIC-ART-088",),
    "CEIC-093": ("CEIC-ART-090",),
    "CEIC-094": ("CEIC-ART-091",),
}

REQUIRED_MEMORY_ARTIFACTS = (
    "CEIC-ART-020",
    "CEIC-ART-022",
    "CEIC-ART-023",
    "CEIC-ART-024",
    "CEIC-ART-026",
    "CEIC-ART-027",
    "CEIC-ART-028",
    "CEIC-ART-029",
    "CEIC-ART-030",
    "CEIC-ART-031",
    "CEIC-ART-032",
    "CEIC-ART-033",
    "CEIC-ART-034",
    "CEIC-ART-035",
    "CEIC-ART-036",
    "CEIC-ART-037",
    "CEIC-ART-038",
    "CEIC-ART-039",
    "CEIC-ART-040",
    "CEIC-ART-041",
    "CEIC-ART-042",
    "CEIC-ART-043",
    "CEIC-ART-044",
)


@dataclass(frozen=True)
class Anchor:
    slice_id: str
    path: str
    search_key: str
    state: str
    surface: str


SOURCE_ANCHORS = (
    Anchor(
        "CEIC-010",
        "project/src/core/memory/sharded_memory_accounting_ledger.hpp",
        "CEIC-010",
        "implemented",
        "sharded accounting ledger",
    ),
    Anchor(
        "CEIC-011",
        "project/src/core/memory/hierarchical_memory_budget_ledger.hpp",
        "CEIC-011",
        "implemented",
        "hierarchical reservation ledger",
    ),
    Anchor(
        "CEIC-012",
        "project/src/core/memory/reservation_backed_memory_resource.cpp",
        "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS",
        "implemented",
        "reservation-backed arenas",
    ),
    Anchor(
        "CEIC-013",
        "project/src/core/memory/typed_slab_pool.cpp",
        "CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS",
        "implemented",
        "typed slabs and size classes",
    ),
    Anchor(
        "CEIC-014",
        "project/src/core/memory/thread_local_memory_cache.cpp",
        "CEIC-014_THREAD_LOCAL_PER_CORE_NUMA_CACHE",
        "implemented",
        "thread-local per-core cache",
    ),
    Anchor(
        "CEIC-015",
        "project/src/core/memory/thread_local_memory_cache.cpp",
        "CEIC-015_REMOTE_FREE_QUEUES_OWNERSHIP_RECONCILIATION",
        "implemented",
        "remote-free reconciliation",
    ),
    Anchor(
        "CEIC-016",
        "project/src/core/memory/foreign_memory_reservation.cpp",
        "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE",
        "implemented",
        "foreign/native memory reservation",
    ),
    Anchor(
        "CEIC-017",
        "project/src/core/memory/memory_pressure_response.cpp",
        "CEIC-017_MEMORY_PRESSURE_STATE_MACHINE",
        "implemented",
        "pressure state machine",
    ),
    Anchor(
        "CEIC-018",
        "project/src/core/memory/temp_workspace_lifecycle.hpp",
        "TempWorkspaceBudgetReservationEvidence",
        "implemented",
        "secure temp spill reservation",
    ),
    Anchor(
        "CEIC-019",
        "project/src/storage/page/page_cache.cpp",
        "MMCH_PAGE_CACHE_SHARDED_FRAME_TABLE",
        "implemented",
        "page-cache frame ownership",
    ),
    Anchor(
        "CEIC-020",
        "project/src/core/memory/result_cursor_plan_memory_governance.cpp",
        "CEIC-020_RESULT_CURSOR_PLAN_CACHE_PREPARED_MEMORY_GOVERNANCE",
        "implemented",
        "cursor, plan-cache, prepared memory governance",
    ),
    Anchor(
        "CEIC-021",
        "project/tools/ceic_memory_source_gate.py",
        "CEIC_021_MEMORY_SOURCE_GATE",
        "focused_test_covered",
        "raw hot-path memory source gate",
    ),
    Anchor(
        "CEIC-022",
        "project/tools/ceic_memory_invariant_gate.py",
        "CEIC_022_MEMORY_INVARIANT_GATE",
        "focused_test_covered",
        "memory invariants and lane validation",
    ),
    Anchor(
        "CEIC-023",
        "project/src/core/memory/memory_support_bundle.cpp",
        "CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY",
        "implemented",
        "bounded low-memory support bundle",
    ),
    Anchor(
        "CEIC-024",
        "project/tools/ceic_memory_readiness_manifest.py",
        "CEIC_024_MEMORY_READINESS_MANIFEST_TOOL",
        "generated",
        "memory readiness manifest producer",
    ),
    Anchor(
        "CEIC-025",
        "project/src/core/memory/memory_fairness_scheduler.hpp",
        "CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS",
        "implemented",
        "multi-tenant memory fairness scheduler",
    ),
    Anchor(
        "CEIC-026",
        "project/src/core/memory/memory_class_policy_lease.hpp",
        "CEIC-026_MEMORY_CLASS_POLICY_LEASES",
        "implemented",
        "memory class policies and budget leases",
    ),
    Anchor(
        "CEIC-027",
        "project/src/core/memory/plugin_native_memory_sandbox.hpp",
        "CEIC-027_PLUGIN_NATIVE_MEMORY_SANDBOX",
        "implemented",
        "plugin/UDR native memory sandbox",
    ),
    Anchor(
        "CEIC-028",
        "project/src/core/memory/memory_fragmentation_profiler.hpp",
        "CEIC-028_FRAGMENTATION_PROFILER_DIFF",
        "implemented",
        "expanded typed arena fragmentation profiler diff",
    ),
    Anchor(
        "CEIC-029",
        "project/src/core/memory/memory_adaptive_batch_working_set.hpp",
        "CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY",
        "implemented",
        "adaptive batch working-set and locality coupling",
    ),
)

FOCUSED_CTESTS = (
    "ceic_010_sharded_memory_accounting_ledger_gate",
    "ceic_011_hierarchical_memory_budget_ledger_gate",
    "ceic_012_query_operator_planner_parser_arenas_gate",
    "ceic_013_typed_slab_pool_gate",
    "ceic_014_thread_local_per_core_cache_gate",
    "ceic_015_remote_free_queue_gate",
    "ceic_016_foreign_memory_reservation_gate",
    "ceic_017_memory_pressure_state_machine_gate",
    "ceic_018_secure_temp_spill_reservation_gate",
    "ceic_019_page_cache_sharded_frame_pool_gate",
    "ceic_020_result_cursor_plan_memory_governance_gate",
    "ceic_021_memory_source_gate",
    "ceic_022_memory_invariant_static_gate",
    "ceic_022_memory_reservation_model_gate",
    "ceic_023_memory_support_bundle_protected_gate",
    "ceic_024_memory_readiness_manifest_gate",
    "ceic_025_multi_tenant_memory_fairness_gate",
    "ceic_026_memory_class_policy_lease_gate",
    "ceic_027_plugin_native_memory_sandbox_gate",
    "ceic_028_fragmentation_profiler_diff_gate",
    "ceic_029_adaptive_batch_working_set_locality_gate",
)

FOCUSED_TARGETS = (
    "ceic_010_sharded_memory_accounting_ledger_gate",
    "ceic_011_hierarchical_memory_budget_ledger_gate",
    "ceic_012_query_operator_planner_parser_arenas_gate",
    "ceic_013_typed_slab_pool_gate",
    "ceic_014_thread_local_per_core_cache_gate",
    "ceic_015_remote_free_queue_gate",
    "ceic_016_foreign_memory_reservation_gate",
    "ceic_017_memory_pressure_state_machine_gate",
    "ceic_018_secure_temp_spill_reservation_gate",
    "ceic_019_page_cache_sharded_frame_pool_gate",
    "ceic_020_result_cursor_plan_memory_governance_gate",
    "ceic_021_memory_source_gate_check",
    "ceic_022_memory_invariant_static_gate_check",
    "ceic_022_memory_reservation_model_gate",
    "ceic_023_memory_support_bundle_protected_gate",
    "ceic_024_memory_readiness_manifest_gate_check",
    "ceic_025_multi_tenant_memory_fairness_gate",
    "ceic_026_memory_class_policy_lease_gate",
    "ceic_027_plugin_native_memory_sandbox_gate",
    "ceic_028_fragmentation_profiler_diff_gate",
    "ceic_029_adaptive_batch_working_set_locality_gate",
)

CONTROL_INPUTS = (
    EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv",
    EXECUTION_PLAN / "ARTIFACT_INDEX.csv",
    EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv",
    EXECUTION_PLAN / "CEIC_FINDING_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv",
    EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv",
    EXECUTION_PLAN / "CEIC_IMPLEMENTATION_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "EVIDENCE_MANIFEST_SCHEMA.md",
    EXECUTION_PLAN / "README.md",
    EXECUTION_PLAN / "INTERFACE_CONTRACTS.md",
    pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt"),
    pathlib.Path("project/tools/ceic_memory_source_gate.py"),
    pathlib.Path("project/tools/ceic_memory_source_gate_allowlist.json"),
    pathlib.Path("project/tools/ceic_memory_invariant_gate.py"),
    pathlib.Path("project/tools/ceic_memory_verification_lanes.json"),
    pathlib.Path("project/cmake/CommercialReadinessProductionBuildGateMatrix.cmake"),
)


class ManifestError(Exception):
    pass


def normalize_status(value: str) -> str:
    return value.strip().lower().replace(" ", "_").replace("-", "_")


def read_csv(repo_root: pathlib.Path, rel: pathlib.Path) -> list[dict[str, str]]:
    path = repo_root / rel
    if not path.exists():
        raise ManifestError(f"missing required CSV: {rel}")
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    return [{key: value or "" for key, value in row.items()} for row in rows]


def read_text(repo_root: pathlib.Path, rel: pathlib.Path) -> str:
    path = repo_root / rel
    if not path.exists():
        raise ManifestError(f"missing required file: {rel}")
    return path.read_text(encoding="utf-8")


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def render_json(data: dict[str, Any]) -> str:
    return json.dumps(data, indent=2, ensure_ascii=True) + "\n"


def git_value(repo_root: pathlib.Path, args: list[str], fallback: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return fallback
    value = result.stdout.strip()
    return value or fallback


def index_by(rows: Iterable[dict[str, str]], key: str, label: str) -> dict[str, dict[str, str]]:
    indexed: dict[str, dict[str, str]] = {}
    duplicates: set[str] = set()
    for row in rows:
        value = row.get(key, "").strip()
        if not value:
            raise ManifestError(f"{label} contains blank {key}")
        if value in indexed:
            duplicates.add(value)
        indexed[value] = row
    if duplicates:
        raise ManifestError(f"{label} duplicate {key}: {', '.join(sorted(duplicates))}")
    return indexed


def require_search_key(repo_root: pathlib.Path, rel: pathlib.Path, search_key: str) -> None:
    text = read_text(repo_root, rel)
    if search_key not in text:
        raise ManifestError(f"{rel} missing search key {search_key}")


def artifact_exists(repo_root: pathlib.Path, row: dict[str, str]) -> bool:
    raw_path = row.get("path", "").strip()
    if not raw_path:
        return False
    if any(char in raw_path for char in "*?["):
        return any(repo_root.glob(raw_path))
    return (repo_root / raw_path).exists()


def artifact_digest(repo_root: pathlib.Path, row: dict[str, str]) -> str:
    raw_path = row.get("path", "").strip()
    path = repo_root / raw_path
    if any(char in raw_path for char in "*?["):
        matches = sorted(repo_root.glob(raw_path))
        digest = hashlib.sha256()
        for match in matches:
            rel = match.relative_to(repo_root).as_posix()
            digest.update(rel.encode("utf-8"))
            digest.update(b"\0")
            digest.update(sha256_file(match).encode("ascii"))
            digest.update(b"\0")
        return digest.hexdigest()
    return sha256_file(path)


def validate_execution_plan_inputs(
    repo_root: pathlib.Path,
    *,
    writing: bool,
) -> tuple[
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
    list[dict[str, str]],
    list[str],
    list[str],
]:
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv"), "slice_id", "CEIC_STATUS_MATRIX.csv")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id", "ARTIFACT_INDEX.csv")
    metrics = read_csv(repo_root, EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv")

    for slice_id in MEMORY_COMPLETED_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status not in COMPLETE_STATUS:
            raise ManifestError(f"{slice_id} must be complete before CEIC-024 manifest generation")
    if normalize_status(tracker.get("CEIC-024", {}).get("status", "")) not in COMPLETE_STATUS:
        raise ManifestError("CEIC-024 must be marked complete only with a current generated manifest")
    completed_extension_slices: list[str] = []
    pending_extension_slices: list[str] = []
    for slice_id in MEMORY_EXTENSION_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status in COMPLETE_STATUS:
            completed_extension_slices.append(slice_id)
        elif status == "pending":
            pending_extension_slices.append(slice_id)
        else:
            raise ManifestError(f"{slice_id} must be pending or complete for CEIC-024 manifest refresh")
    validate_integrated_release_proof(repo_root, tracker, artifacts, writing=writing)

    manifest_row = artifacts.get("CEIC-ART-011")
    if not manifest_row:
        raise ManifestError("CEIC-ART-011 manifest artifact row missing")
    if manifest_row.get("path", "").strip() != DEFAULT_MANIFEST.as_posix():
        raise ManifestError("CEIC-ART-011 path must be the CEIC-024 generated manifest under execution_plan artifacts")
    if normalize_status(manifest_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-011 must be present/generated when CEIC-024 is complete")
    if not writing and not artifact_exists(repo_root, manifest_row):
        raise ManifestError("CEIC-ART-011 is present but the manifest file is missing")

    for artifact_id in REQUIRED_MEMORY_ARTIFACTS:
        row = artifacts.get(artifact_id)
        if row is None:
            raise ManifestError(f"{artifact_id} required for CEIC-024 but missing from ARTIFACT_INDEX.csv")
        if normalize_status(row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError(f"{artifact_id} must be present before CEIC-024 manifest generation")
        if not artifact_exists(repo_root, row):
            raise ManifestError(f"{artifact_id} is marked present but missing: {row.get('path', '')}")

    memory_metrics = [
        row
        for row in metrics
        if "memory" in row.get("subsystem", "").lower()
        or row.get("metric_family", "").startswith(("page_cache_", "temp_spill_"))
    ]
    if not memory_metrics:
        raise ManifestError("METRICS_PRODUCER_COVERAGE_MATRIX.csv has no memory metric rows")
    for row in memory_metrics:
        if not row.get("required_producer_path", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no producer path")
        if not row.get("validation_gate", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no validation gate")

    return tracker, artifacts, memory_metrics, completed_extension_slices, pending_extension_slices


def validate_integrated_release_proof(
    repo_root: pathlib.Path,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    *,
    writing: bool,
) -> None:
    for slice_id in INTEGRATED_RELEASE_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status not in COMPLETE_STATUS:
            raise ManifestError(f"{slice_id} integrated proof must be complete for CEIC-024 beta release validation")
        if not tracker.get(slice_id, {}).get("acceptance", "").strip():
            raise ManifestError(f"{slice_id} integrated proof acceptance text is required")
        for artifact_id in INTEGRATED_RELEASE_ARTIFACTS[slice_id]:
            row = artifacts.get(artifact_id)
            if row is None:
                raise ManifestError(f"{artifact_id} required for {slice_id} integrated proof but missing")
            if row.get("slice_id", "").strip() != slice_id:
                raise ManifestError(f"{artifact_id} must belong to {slice_id}")
            if normalize_status(row.get("status", "")) not in PRESENT_STATUS:
                raise ManifestError(f"{artifact_id} must be present for {slice_id} integrated proof")
            if not writing and not artifact_exists(repo_root, row):
                raise ManifestError(f"{artifact_id} is present but missing: {row.get('path', '')}")


def validate_static_inputs(repo_root: pathlib.Path) -> None:
    for anchor in SOURCE_ANCHORS:
        require_search_key(repo_root, pathlib.Path(anchor.path), anchor.search_key)

    cmake = read_text(repo_root, pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt"))
    for test_name in FOCUSED_CTESTS + ("consolidated_enterprise_proof_control_gate",):
        if f"NAME {test_name}" not in cmake:
            raise ManifestError(f"CMakeLists.txt missing add_test for {test_name}")
    for target_name in FOCUSED_TARGETS + ("consolidated_enterprise_proof_control_gate_check",):
        if target_name not in cmake:
            raise ManifestError(f"CMakeLists.txt missing target/reference for {target_name}")

    production_gate = read_text(
        repo_root, pathlib.Path("project/cmake/CommercialReadinessProductionBuildGateMatrix.cmake")
    )
    for phrase in (
        "CRP-PRODUCTION-BUILD-SAFETY-GATE-MATRIX",
        "clean_release TRUE",
        "fixture_auth FALSE",
        "cluster_stub_claim FALSE",
        "external_provider_claim TRUE",
    ):
        if phrase not in production_gate:
            raise ManifestError(f"production gate matrix missing {phrase}")

    lane_manifest_path = repo_root / "project/tools/ceic_memory_verification_lanes.json"
    lanes = json.loads(lane_manifest_path.read_text(encoding="utf-8"))
    if lanes.get("authority_boundary") != AUTHORITY_BOUNDARY_TOKEN:
        raise ManifestError("CEIC-022 lane manifest authority boundary token mismatch")
    soak_durations = {
        entry.get("duration_hours")
        for entry in lanes.get("soak_lanes", [])
        if isinstance(entry, dict)
    }
    if soak_durations != {24, 72}:
        raise ManifestError("CEIC-022 lane manifest must define exactly 24h and 72h soak lanes")
    dynamic_tools = {
        entry.get("tool_preference")
        for entry in lanes.get("dynamic_analysis_lanes", [])
        if isinstance(entry, dict)
    }
    if "application_verifier" not in dynamic_tools:
        raise ManifestError("CEIC-022 lane manifest must include Windows Application Verifier coverage")

    adaptive_source = read_text(
        repo_root,
        pathlib.Path("project/src/core/memory/memory_adaptive_batch_working_set.hpp"),
    )
    for phrase in (
        "clock_replacement_policy_evaluated",
        "lru2_replacement_policy_evaluated",
        "arc_replacement_policy_evaluated",
        "selected_replacement_policy",
    ):
        if phrase not in adaptive_source:
            raise ManifestError(f"CEIC-029 adaptive working-set source missing {phrase}")


def collect_input_records(
    repo_root: pathlib.Path,
    artifacts: dict[str, dict[str, str]],
) -> tuple[list[dict[str, Any]], str]:
    rel_paths: set[pathlib.Path] = {pathlib.Path(anchor.path) for anchor in SOURCE_ANCHORS}
    rel_paths.update(CONTROL_INPUTS)
    rel_paths.add(EXECUTION_PLAN / "artifacts/CEIC-024_MEMORY_READINESS_MANIFEST_EVIDENCE.md")

    for artifact_id in REQUIRED_MEMORY_ARTIFACTS:
        raw_path = artifacts[artifact_id]["path"].strip()
        if any(char in raw_path for char in "*?["):
            for match in sorted(repo_root.glob(raw_path)):
                rel_paths.add(match.relative_to(repo_root))
        else:
            rel_paths.add(pathlib.Path(raw_path))
    for artifact_ids in INTEGRATED_RELEASE_ARTIFACTS.values():
        for artifact_id in artifact_ids:
            raw_path = artifacts[artifact_id]["path"].strip()
            if any(char in raw_path for char in "*?["):
                for match in sorted(repo_root.glob(raw_path)):
                    rel_paths.add(match.relative_to(repo_root))
            else:
                rel_paths.add(pathlib.Path(raw_path))

    rel_paths.discard(DEFAULT_MANIFEST)
    records: list[dict[str, Any]] = []
    digest = hashlib.sha256()
    for rel_path in sorted(rel_paths, key=lambda path: path.as_posix()):
        path = repo_root / rel_path
        if not path.exists():
            raise ManifestError(f"manifest input missing: {rel_path.as_posix()}")
        file_hash = sha256_file(path)
        rel_text = rel_path.as_posix()
        records.append(
            {
                "path": rel_text,
                "sha256": file_hash,
                "bytes": path.stat().st_size,
            }
        )
        digest.update(rel_text.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\0")

    return records, digest.hexdigest()


def build_manifest(
    repo_root: pathlib.Path,
    *,
    writing: bool = False,
    carried: dict[str, str] | None = None,
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    carried = carried or {}
    tracker, artifacts, memory_metrics, completed_extension_slices, pending_extension_slices = (
        validate_execution_plan_inputs(repo_root, writing=writing)
    )
    validate_static_inputs(repo_root)
    input_records, input_digest = collect_input_records(repo_root, artifacts)

    source_commit = carried.get("source_commit") or git_value(repo_root, ["rev-parse", "HEAD"], "unknown")
    source_branch = carried.get("source_branch") or git_value(
        repo_root, ["rev-parse", "--abbrev-ref", "HEAD"], "unknown"
    )
    generated_at = carried.get("generated_at_utc") or git_value(
        repo_root, ["show", "-s", "--format=%cI", "HEAD"], "1970-01-01T00:00:00+00:00"
    )
    manifest_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"scratchbird.ceic.024.memory:{input_digest}"))

    source_anchors = []
    for anchor in SOURCE_ANCHORS:
        path = repo_root / anchor.path
        source_anchors.append(
            {
                "slice_id": anchor.slice_id,
                "path": anchor.path,
                "search_key": anchor.search_key,
                "state": anchor.state,
                "surface": anchor.surface,
                "sha256": sha256_file(path),
            }
        )

    artifact_rows = []
    for artifact_id in REQUIRED_MEMORY_ARTIFACTS:
        row = artifacts[artifact_id]
        artifact_rows.append(
            {
                "artifact_id": artifact_id,
                "slice_id": row["slice_id"],
                "kind": row["artifact_kind"],
                "path": row["path"],
                "sha256": artifact_digest(repo_root, row),
                "state": "present",
                "runtime_proof_state": (
                    "focused_runtime_or_generated_gate_evidence"
                    if row["path"].startswith("project/")
                    else "supporting_evidence_not_runtime_authority"
                ),
            }
        )

    lanes = json.loads((repo_root / "project/tools/ceic_memory_verification_lanes.json").read_text(encoding="utf-8"))
    soak_lanes = [
        {
            "id": entry["id"],
            "kind": entry["kind"],
            "duration_hours": entry["duration_hours"],
            "platforms": entry["platforms"],
            "state": "defined_for_CEIC-093_execution",
            "artifact_policy": entry["artifact_policy"],
        }
        for entry in lanes["soak_lanes"]
    ]
    platforms = sorted(
        {
            platform
            for section in (
                "sanitizer_lanes",
                "dynamic_analysis_lanes",
                "static_analysis_lanes",
                "fuzz_lanes",
                "soak_lanes",
            )
            for entry in lanes.get(section, [])
            for platform in entry.get("platforms", [])
        }
    )

    metric_rows = []
    for row in memory_metrics:
        status = normalize_status(row.get("status", ""))
        metric_rows.append(
            {
                "metric_family": row["metric_family"],
                "subsystem": row["subsystem"],
                "required_producer_path": row["required_producer_path"],
                "operation_path": row["operation_path"],
                "support_bundle_path": row["support_bundle_path"],
                "validation_gate": row["validation_gate"],
                "matrix_status": row["status"],
                "manifest_state": (
                    "focused_support_bundle_producer_complete"
                    if status in COMPLETE_STATUS
                    else "indexed_pending_runtime_producer_or_integrated_closure"
                ),
            }
        )

    integrated_release_proof = []
    for slice_id in INTEGRATED_RELEASE_SLICES:
        integrated_release_proof.append(
            {
                "slice_id": slice_id,
                "title": tracker[slice_id]["title"],
                "status": normalize_status(tracker[slice_id]["status"]),
                "artifact_ids": list(INTEGRATED_RELEASE_ARTIFACTS[slice_id]),
                "ceic_024_state": "complete_integrated_release_proof_required_for_beta_release",
            }
        )

    return {
        "schema_id": "scratchbird.ceic.evidence_manifest",
        "schema_version": 1,
        "manifest_kind": "memory_readiness_manifest",
        "search_key": "CEIC_024_MEMORY_READINESS_MANIFEST",
        "manifest_uuid": manifest_uuid,
        "subsystem": "memory",
        "slice_ids": list(MEMORY_MANIFEST_SLICES) + completed_extension_slices,
        "source_commit": source_commit,
        "source_branch": source_branch,
        "source_evidence_digest": input_digest,
        "generated_at_utc": generated_at,
        "generated_by": "project/tools/ceic_memory_readiness_manifest.py#CEIC_024_MEMORY_READINESS_MANIFEST_TOOL",
        "build_profile": "focused_ceic_memory_manifest",
        "authority_boundary": {
            "boundary_token": AUTHORITY_BOUNDARY_TOKEN,
            "mga_finality_authority": False,
            "transaction_finality_authority": False,
            "visibility_authority": False,
            "authorization_security_authority": False,
            "recovery_authority": False,
            "parser_authority": False,
            "reference_authority": False,
            "wal_recovery_authority": False,
            "support_bundle_authority": False,
            "benchmark_authority": False,
            "optimizer_plan_authority": False,
            "index_finality_authority": False,
            "agent_action_authority": False,
        },
        "readiness_state": {
            "memory_manifest": "generated",
            "implemented_memory_slice_range": "CEIC-010..CEIC-024 plus completed extension slices",
            "completed_memory_extension_slices": completed_extension_slices,
            "ceic_024_gate": "generated_and_validated",
            "docs_alone_runtime_proof": False,
            "production_claim": "blocked_by_memory_manifest_scope_not_product_production_claim",
            "benchmark_clean_claim": "blocked_by_memory_manifest_scope_not_benchmark_dominance_claim",
            "integrated_readiness": "complete_via_integrated_release_evidence",
            "cluster_production_claim": "blocked_external_provider_only",
            "pending_memory_slices": pending_extension_slices,
            "integrated_release_proof": integrated_release_proof,
        },
        "production_test_separation": {
            "fixtures_enabled": False,
            "synthetic_evidence_enabled": False,
            "forced_collision_hooks_enabled": False,
            "debug_only_paths_enabled": False,
            "cluster_stub_enabled": False,
            "relaxed_metric_mode_enabled": False,
            "production_cluster_claims_enabled_without_external_provider": False,
            "production_gate_matrix": {
                "path": "project/cmake/CommercialReadinessProductionBuildGateMatrix.cmake",
                "search_key": "CRP-PRODUCTION-BUILD-SAFETY-GATE-MATRIX",
                "state": "CMake-visible focused gate defined",
            },
        },
        "platform": {
            "os": "declared_cross_platform_lanes",
            "version": "CEIC-094_complete_cross_platform_release_proof",
            "arch": "declared_by_CEIC-022_lane_manifest",
            "compiler": "CMake toolchain selected by focused validation build",
            "declared_platforms": platforms,
            "cross_platform_release_proof_state": "complete_CEIC-094",
        },
        "source_anchors": source_anchors,
        "input_records": input_records,
        "artifact_evidence": artifact_rows,
        "build": {
            "cmake_file": "project/tests/consolidated_enterprise/CMakeLists.txt",
            "focused_targets": list(FOCUSED_TARGETS),
            "focused_ctests": list(FOCUSED_CTESTS),
            "proof_control_target": "consolidated_enterprise_proof_control_gate_check",
            "proof_control_ctest": "consolidated_enterprise_proof_control_gate",
            "state": "CMake-visible focused gates defined; current run evidence remains external validation output",
        },
        "soak_evidence": {
            "state": "complete_CEIC-093_integrated_reliability_security_proof",
            "lane_manifest": "project/tools/ceic_memory_verification_lanes.json",
            "authority_boundary": AUTHORITY_BOUNDARY_TOKEN,
            "lanes": soak_lanes,
        },
        "support_bundle_evidence": {
            "state": "focused_memory_support_bundle_implemented",
            "artifact": "CEIC-ART-038",
            "source_anchor": "project/src/core/memory/memory_support_bundle.cpp#CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY",
            "integrated_signed_chained_bundle_state": "complete_CEIC-091",
            "authority": "observability_evidence_only",
        },
        "metrics_evidence": {
            "state": "memory_metric_rows_indexed_with_pending_runtime_producer_closure_preserved",
            "matrix": "project/tests/release_evidence/consolidated_enterprise_public_evidence/METRICS_PRODUCER_COVERAGE_MATRIX.csv",
            "rows": metric_rows,
        },
        "production_gate_evidence": {
            "state": "production_test_separation_gate_defined_and_required",
            "artifact": "CEIC-ART-024",
            "claim": "memory_manifest_does_not_enable_cluster_or_production_live_claims",
        },
    }


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"{path} is not deterministic JSON/YAML-subset manifest: {exc}") from exc
    if not isinstance(data, dict):
        raise ManifestError(f"{path} must contain a JSON object")
    return data


def validate_manifest_semantics(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema_id") != "scratchbird.ceic.evidence_manifest":
        errors.append("schema_id mismatch")
    if data.get("search_key") != "CEIC_024_MEMORY_READINESS_MANIFEST":
        errors.append("search_key mismatch")
    if data.get("manifest_kind") != "memory_readiness_manifest":
        errors.append("manifest_kind mismatch")
    if data.get("subsystem") != "memory":
        errors.append("subsystem must be memory")
    slice_ids = data.get("slice_ids")
    if not isinstance(slice_ids, list) or slice_ids[: len(MEMORY_MANIFEST_SLICES)] != list(MEMORY_MANIFEST_SLICES):
        errors.append("slice_ids must start with CEIC-010..CEIC-024")
    else:
        for slice_id in slice_ids[len(MEMORY_MANIFEST_SLICES):]:
            if slice_id not in MEMORY_EXTENSION_SLICES:
                errors.append(f"unexpected memory extension slice in manifest: {slice_id}")
    for field in ("source_commit", "source_branch", "source_evidence_digest", "generated_at_utc", "generated_by"):
        if not data.get(field):
            errors.append(f"{field} is required")

    boundary = data.get("authority_boundary", {})
    if boundary.get("boundary_token") != AUTHORITY_BOUNDARY_TOKEN:
        errors.append("authority boundary token mismatch")
    for key, value in boundary.items():
        if key.endswith("_authority") or key == "wal_recovery_authority":
            if value is not False:
                errors.append(f"{key} must be false")

    readiness = data.get("readiness_state", {})
    if readiness.get("docs_alone_runtime_proof") is not False:
        errors.append("docs alone must not be runtime proof")
    for field in ("production_claim", "benchmark_clean_claim", "cluster_production_claim"):
        value = str(readiness.get(field, ""))
        if not value.startswith("blocked"):
            errors.append(f"static claim without generated/integrated proof: {field}={value}")
    if readiness.get("integrated_readiness") != "complete_via_integrated_release_evidence":
        errors.append("integrated readiness must reflect complete integrated release evidence")

    integrated_ids = {
        row.get("slice_id"): row.get("status")
        for row in readiness.get("integrated_release_proof", [])
        if isinstance(row, dict)
    }
    for slice_id in INTEGRATED_RELEASE_SLICES:
        if integrated_ids.get(slice_id) not in COMPLETE_STATUS:
            errors.append(f"{slice_id} integrated proof must be complete")

    separation = data.get("production_test_separation", {})
    for key in (
        "fixtures_enabled",
        "synthetic_evidence_enabled",
        "forced_collision_hooks_enabled",
        "debug_only_paths_enabled",
        "cluster_stub_enabled",
        "relaxed_metric_mode_enabled",
        "production_cluster_claims_enabled_without_external_provider",
    ):
        if separation.get(key) is not False:
            errors.append(f"production/test separation {key} must be false")

    if data.get("soak_evidence", {}).get("state") != "complete_CEIC-093_integrated_reliability_security_proof":
        errors.append("soak evidence must reflect complete CEIC-093 integrated proof")
    if data.get("platform", {}).get("cross_platform_release_proof_state") != "complete_CEIC-094":
        errors.append("cross-platform proof must reflect complete CEIC-094 proof")
    if data.get("support_bundle_evidence", {}).get("integrated_signed_chained_bundle_state") != "complete_CEIC-091":
        errors.append("integrated support bundle proof must reflect complete CEIC-091 proof")

    return errors


def validate_manifest(repo_root: pathlib.Path, manifest_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not manifest_path.exists():
        return [f"manifest missing: {manifest_path}"]
    try:
        actual = load_manifest(manifest_path)
    except ManifestError as exc:
        return [str(exc)]
    errors.extend(validate_manifest_semantics(actual))

    carried = {
        "source_commit": str(actual.get("source_commit", "")),
        "source_branch": str(actual.get("source_branch", "")),
        "generated_at_utc": str(actual.get("generated_at_utc", "")),
    }
    try:
        expected = build_manifest(repo_root, writing=False, carried=carried)
    except ManifestError as exc:
        errors.append(str(exc))
        return errors

    if render_json(actual) != render_json(expected):
        errors.append("stale manifest differs from generated CEIC-024 proof")
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--write", action="store_true", help="write the generated manifest before validating it")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest
    if not manifest_path.is_absolute():
        manifest_path = repo_root / manifest_path

    try:
        if args.write:
            data = build_manifest(repo_root, writing=True)
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest_path.write_text(render_json(data), encoding="utf-8")
        errors = validate_manifest(repo_root, manifest_path)
    except ManifestError as exc:
        errors = [str(exc)]

    if errors:
        for error in errors:
            print(f"ceic_memory_readiness_manifest=fail:{error}", file=sys.stderr)
        return 1
    try:
        display_path = manifest_path.relative_to(repo_root).as_posix()
    except ValueError:
        display_path = manifest_path.as_posix()
    print(f"ceic_memory_readiness_manifest=pass:{display_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
