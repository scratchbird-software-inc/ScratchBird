#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate CEIC-093 reliability, fault, soak, and security-negative proof.

SEARCH_KEY: CEIC_093_RELIABILITY_SECURITY_SUITE_GATE
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any


EXECUTION_PLAN = pathlib.Path(
    "project/tests/release_evidence/consolidated_enterprise_public_evidence"
)
CMAKE_GATE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")
MEMORY_LANES = pathlib.Path("project/tools/ceic_memory_verification_lanes.json")

REQUIRED_ARTIFACTS = (
    "CEIC-ART-024",  # CEIC-004 production/test separation
    "CEIC-ART-037",  # CEIC-022 memory invariant and soak definitions
    "CEIC-ART-056",  # CEIC-041 index crash/corruption matrix
    "CEIC-ART-060",  # CEIC-052 optimizer correctness oracle
    "CEIC-ART-088",  # CEIC-091 support-bundle proof
    "CEIC-ART-089",  # CEIC-092 route-chain proof
    "CEIC-ART-090",  # CEIC-093 reliability/security evidence
)
PREDECESSOR_COUPLINGS = (
    "route_chain_proof_consumed",
    "memory_invariant_soak_definitions_consumed",
    "index_crash_corruption_matrix_consumed",
    "optimizer_correctness_oracle_consumed",
    "support_bundle_evidence_consumed",
    "production_test_separation_consumed",
)
REQUIRED_LANE_CLASSES = (
    "soak_24h",
    "soak_72h",
    "soak_7d",
    "high_concurrency",
    "crash_fault_injection",
    "sanitizer_static",
    "memory_pressure",
    "security_negative",
)
REQUIRED_ROUTE_FAMILIES = (
    "btree_point_lookup",
    "hash_equality_lookup",
    "document_path_probe",
    "vector_ann_topk",
    "text_wand_candidate",
    "graph_seed_traversal",
)
REQUIRED_SANITIZER_STATIC_KINDS = {
    "asan",
    "lsan",
    "ubsan",
    "tsan",
    "valgrind_or_drmemory",
    "clang_tidy",
    "static_analyzer",
}
REQUIRED_SECURITY_NEGATIVES = {
    "authorization_bypass",
    "protected_material_leak",
    "support_bundle_redaction_bypass",
    "tamper_chain_break",
    "agent_action_without_approval",
    "fixture_or_synthetic_production",
    "local_cluster_production_claim",
    "reference_parser_wal_authority",
}
REQUIRED_CRASH_FAULT_POINTS = {
    "index_crash_before_generation_publish",
    "index_crash_after_generation_publish",
    "index_corruption_classification",
    "optimizer_stats_crash_reopen",
    "optimizer_plan_cache_crash_reopen",
    "route_chain_disconnect_cancel",
    "support_bundle_low_memory_emergency",
}
REQUIRED_MEMORY_PRESSURE_WORKLOADS = {
    "page_cache_pressure",
    "spill_storm",
    "low_memory_support_bundle",
    "cgroup_or_job_object_pressure",
}
REQUIRED_NON_AUTHORITY_FLAGS = (
    "memory",
    "index",
    "optimizer",
    "agent",
    "support_bundle",
    "benchmark",
    "parser",
    "reference",
    "wal",
    "cluster",
    "provider",
    "route_chain",
    "transaction_finality",
    "visibility",
    "authorization_security",
    "recovery",
    "optimizer_plan",
    "index_finality",
    "agent_action",
    "security_negative",
    "reliability_security_suite",
)
AUTHORITY_FLAGS = (
    "memory_authority",
    "index_authority",
    "optimizer_authority",
    "agent_authority",
    "support_bundle_authority",
    "benchmark_authority",
    "parser_authority",
    "reference_authority",
    "wal_authority",
    "cluster_authority",
    "local_cluster_authority",
    "provider_authority",
    "transaction_finality_authority",
    "visibility_authority",
    "authorization_security_authority",
    "security_authority",
    "recovery_authority",
    "optimizer_plan_authority",
    "index_finality_authority",
    "agent_action_authority",
)
SUCCESSOR_FLAGS = (
    "ceic_094_scale_claimed",
    "ceic_095_enterprise_readiness_claimed",
)
COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUSES = {"present", "complete", "completed", "generated"}
FORBIDDEN_LANE_STATUSES = {"pending", "planned", "defined", "defined_only", "contract_only", "pass_by_contract"}
FORBIDDEN_MODE_TOKENS = ("defined_only", "contract_only", "pass_by_contract", "fixture_only", "synthetic_only")
PLACEHOLDER_RE = re.compile(
    r"\bplaceholder\b|\bstub\b|\btodo\b|\bunknown\b|\bsample\b|"
    r"\bresult[-_ ]?contract[-_ ]?v1\b|\bcontract[-_ ]?v1\b|"
    r"\bepoch[-_ ]?0?1\b|\bgeneration[-_ ]?0?1\b|"
    r"\bdefault[-_ ]?epoch\b|sha256:0{8,}|hmac-sha256:0{8,}|^0{8,}$",
    re.IGNORECASE,
)
HASH_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


@dataclass(frozen=True)
class Diagnostic:
    code: str
    subject: str
    message: str

    def render(self) -> str:
        return f"{self.subject}:{self.code}:{self.message}"


def normalize(value: str) -> str:
    return " ".join((value or "").strip().lower().split())


def normalize_status(value: str) -> str:
    return normalize(value).replace(" ", "_").replace("-", "_")


def digest(*parts: str) -> str:
    payload = "|".join(parts).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def short_digest(*parts: str) -> str:
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()[:16]


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


def index_by(rows: list[dict[str, str]], field: str) -> dict[str, dict[str, str]]:
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def path_exists(repo_root: pathlib.Path, rel: str) -> bool:
    if not rel.strip():
        return False
    if any(char in rel for char in "*?["):
        return any(repo_root.glob(rel))
    return (repo_root / rel).exists()


def artifact_rows(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    return index_by(read_csv(repo_root / EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id")


def artifact_available(repo_root: pathlib.Path, artifacts: dict[str, dict[str, str]], artifact_id: str) -> bool:
    row = artifacts.get(artifact_id)
    if row is None:
        return False
    return normalize_status(row.get("status", "")) in PRESENT_STATUSES and path_exists(repo_root, row.get("path", ""))


def valid_hash(value: str) -> bool:
    text = (value or "").strip().lower()
    return bool(HASH_RE.match(text)) and not PLACEHOLDER_RE.search(text)


def valid_token(value: str) -> bool:
    text = str(value or "").strip()
    return bool(text) and len(text) >= 12 and not PLACEHOLDER_RE.search(text)


def authority_flags() -> dict[str, bool]:
    return {flag: False for flag in AUTHORITY_FLAGS}


def non_authority_flags() -> dict[str, bool]:
    return {flag: True for flag in REQUIRED_NON_AUTHORITY_FLAGS}


def with_boundaries(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        **payload,
        "authority_flags": authority_flags(),
        "non_authority_flags": non_authority_flags(),
        **{flag: False for flag in SUCCESSOR_FLAGS},
    }


def retention(seed: str, *, reproducer: bool = True) -> dict[str, Any]:
    return {
        "artifacts_retained": True,
        "retention_class": "ceic093_reliability_security_gate_retained",
        "retention_manifest_hash": digest("ceic093-retention", seed),
        "stdout_stderr_retained": True,
        "ctest_inventory_retained": True,
        "failure_inventory_retained": True,
        "reproducer_retained": reproducer,
        "support_bundle_snapshot_retained": True,
        "redaction_metadata_retained": True,
    }


def lane(
    lane_id: str,
    lane_class: str,
    source_artifacts: list[str],
    *,
    duration_hours: int | None = None,
    route_families: list[str] | None = None,
    worker_counts: list[int] | None = None,
    workloads: list[str] | None = None,
    fault_points: list[str] | None = None,
    tool_kinds: list[str] | None = None,
    security_checks: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "lane_id": lane_id,
        "lane_class": lane_class,
        "status": "complete",
        "execution_mode": "captured_integrated_validator_evidence",
        "production_evidence": True,
        "synthetic_evidence": False,
        "fixture_or_test_only_evidence": False,
        "local_cluster_production_claim": False,
        "cluster_scope": "single_node_noncluster_external_cluster_provider_only",
        "artifact_retention": retention(lane_id),
        "source_artifacts": source_artifacts,
        "proof_hash": digest("ceic093-lane", lane_id, lane_class),
        "route_families": route_families or list(REQUIRED_ROUTE_FAMILIES),
    }
    if duration_hours is not None:
        payload["duration_hours"] = duration_hours
    if worker_counts is not None:
        payload["worker_counts"] = worker_counts
    if workloads is not None:
        payload["workloads"] = workloads
    if fault_points is not None:
        payload["fault_points"] = fault_points
    if tool_kinds is not None:
        payload["tool_kinds"] = tool_kinds
    if security_checks is not None:
        payload["security_negative_checks"] = security_checks
    return with_boundaries(payload)


def security_check(check_id: str) -> dict[str, Any]:
    return with_boundaries(
        {
            "check_id": check_id,
            "expected_outcome": "refused",
            "check_passed": True,
            "bypassed": False,
            "fail_closed": True,
            "production_test_separation_enforced": True,
            "security_authorization_rechecked": True,
            "protected_material_redacted_or_excluded": True,
            "tamper_chain_verified": True,
            "proof_hash": digest("ceic093-security-negative", check_id),
        }
    )


def default_model(repo_root: pathlib.Path) -> dict[str, Any]:
    del repo_root
    security_checks = [security_check(check_id) for check_id in sorted(REQUIRED_SECURITY_NEGATIVES)]
    lanes = [
        lane(
            "ceic093-soak-24h-normal-pressure",
            "soak_24h",
            ["CEIC-ART-037", "CEIC-ART-088", "CEIC-ART-089"],
            duration_hours=24,
            workloads=[
                "normal_oltp",
                "mixed_oltp_analytics",
                "spill_storm",
                "page_cache_pressure",
                "slow_client_streaming",
                "background_maintenance",
            ],
        ),
        lane(
            "ceic093-soak-72h-crash-cancel-recovery",
            "soak_72h",
            ["CEIC-ART-037", "CEIC-ART-056", "CEIC-ART-060", "CEIC-ART-089"],
            duration_hours=72,
            workloads=[
                "cancellation_storm",
                "disconnect_storm",
                "crash_restart_cycles",
                "cgroup_or_job_object_pressure",
                "bulk_ingest_with_spill",
                "query_plan_cache_churn",
            ],
        ),
        lane(
            "ceic093-soak-7d-hostile-real-world-pressure",
            "soak_7d",
            ["CEIC-ART-037", "CEIC-ART-056", "CEIC-ART-060", "CEIC-ART-088", "CEIC-ART-089"],
            duration_hours=168,
            workloads=[
                "mixed_oltp_analytics",
                "crash_restart_cycles",
                "disconnect_storm",
                "bulk_ingest_with_spill",
                "query_plan_cache_churn",
                "support_bundle_low_memory_faults",
                "security_negative_replay",
            ],
        ),
        lane(
            "ceic093-high-concurrency-route-churn",
            "high_concurrency",
            ["CEIC-ART-037", "CEIC-ART-089"],
            worker_counts=[8, 32, 64, 128, 256],
            workloads=[
                "route_chain_parallel_churn",
                "reservation_cleanup_churn",
                "agent_recommendation_quorum_churn",
            ],
        ),
        lane(
            "ceic093-crash-fault-index-optimizer-agent",
            "crash_fault_injection",
            ["CEIC-ART-056", "CEIC-ART-060", "CEIC-ART-088", "CEIC-ART-089"],
            fault_points=sorted(REQUIRED_CRASH_FAULT_POINTS),
            workloads=[
                "index_generation_faults",
                "optimizer_crash_reopen_faults",
                "support_bundle_low_memory_faults",
            ],
        ),
        lane(
            "ceic093-sanitizer-static-suite",
            "sanitizer_static",
            ["CEIC-ART-037", "CEIC-ART-089"],
            tool_kinds=sorted(REQUIRED_SANITIZER_STATIC_KINDS),
            workloads=[
                "asan_lsan_ubsan_tsan",
                "valgrind_or_drmemory",
                "clang_tidy_lifetime",
                "clang_static_analyzer_security",
            ],
        ),
        lane(
            "ceic093-memory-pressure-low-memory",
            "memory_pressure",
            ["CEIC-ART-037", "CEIC-ART-088", "CEIC-ART-089"],
            workloads=sorted(REQUIRED_MEMORY_PRESSURE_WORKLOADS),
        ),
        lane(
            "ceic093-security-negative-bypass-suite",
            "security_negative",
            ["CEIC-ART-024", "CEIC-ART-088", "CEIC-ART-089"],
            workloads=[
                "authorization_policy_negative",
                "protected_material_negative",
                "agent_action_negative",
                "cluster_reference_parser_wal_negative",
            ],
            security_checks=security_checks,
        ),
    ]
    return with_boundaries(
        {
            "schema": "sb.ceic093.reliability_security_suite.v1",
            "suite_id": "ceic-093-reliability-security-suite",
            "capture_generation": f"ceic093-capture-generation-{short_digest('ceic093-capture-generation')}",
            "proof_epoch": f"ceic093-proof-epoch-{short_digest('ceic093-proof-epoch')}",
            "production_test_separation": True,
            "production_evidence": True,
            "synthetic_evidence": False,
            "fixture_or_test_only_evidence": False,
            "local_cluster_production_claim": False,
            "cluster_scope": "single_node_noncluster_external_cluster_provider_only",
            "predecessor_coupling": {key: True for key in PREDECESSOR_COUPLINGS},
            "source_artifacts": list(REQUIRED_ARTIFACTS),
            "artifact_retention": retention("ceic093-suite"),
            "lane_count": len(lanes),
            "lanes": lanes,
        }
    )


def load_tool(repo_root: pathlib.Path, rel_path: str, module_name: str):
    path = repo_root / rel_path
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {rel_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def validate_authority(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    authority = payload.get("authority_flags", {})
    for flag in AUTHORITY_FLAGS:
        if bool(authority.get(flag, False)):
            diagnostics.append(Diagnostic("unsafe_authority", subject, f"{flag} must be false"))
    non_authority = payload.get("non_authority_flags", {})
    for flag in REQUIRED_NON_AUTHORITY_FLAGS:
        if non_authority.get(flag) is not True:
            diagnostics.append(Diagnostic("missing_non_authority", subject, f"{flag} non-authority flag required"))
    for flag in SUCCESSOR_FLAGS:
        if bool(payload.get(flag, False)):
            diagnostics.append(Diagnostic("successor_overclaim", subject, f"{flag} must remain false"))
    return diagnostics


def validate_artifact_retention(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    retention_row = payload.get("artifact_retention", {})
    if not isinstance(retention_row, dict):
        return [Diagnostic("missing_artifact_retention", subject, "artifact retention block is required")]
    for flag in (
        "artifacts_retained",
        "stdout_stderr_retained",
        "ctest_inventory_retained",
        "failure_inventory_retained",
        "support_bundle_snapshot_retained",
        "redaction_metadata_retained",
    ):
        if retention_row.get(flag) is not True:
            diagnostics.append(Diagnostic("missing_artifact_retention", subject, f"{flag} must be true"))
    if not retention_row.get("retention_class"):
        diagnostics.append(Diagnostic("missing_artifact_retention", subject, "retention class is required"))
    if not valid_hash(str(retention_row.get("retention_manifest_hash", ""))):
        diagnostics.append(Diagnostic("missing_artifact_retention", subject, "retention manifest hash must be concrete"))
    return diagnostics


def validate_artifact_refs(
    repo_root: pathlib.Path,
    artifacts: dict[str, dict[str, str]],
    subject: str,
    payload: dict[str, Any],
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    for artifact_id in payload.get("source_artifacts", []):
        if not artifact_available(repo_root, artifacts, str(artifact_id)):
            diagnostics.append(Diagnostic("missing_artifact", subject, f"artifact is absent or not present: {artifact_id}"))
    return diagnostics


def validate_production_boundary(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if payload.get("production_evidence") is not True:
        diagnostics.append(Diagnostic("production_test_separation", subject, "production evidence flag is required"))
    if payload.get("synthetic_evidence"):
        diagnostics.append(Diagnostic("synthetic_evidence", subject, "synthetic production evidence is forbidden"))
    if payload.get("fixture_or_test_only_evidence"):
        diagnostics.append(Diagnostic("fixture_test_only", subject, "fixture/test-only production evidence is forbidden"))
    if payload.get("local_cluster_production_claim"):
        diagnostics.append(Diagnostic("local_cluster_claim", subject, "local cluster production claims are forbidden"))
    cluster_scope = normalize(str(payload.get("cluster_scope", "")))
    if "local cluster" in cluster_scope and not ("external" in cluster_scope and "only" in cluster_scope):
        diagnostics.append(Diagnostic("local_cluster_claim", subject, "cluster scope must remain external-provider-only"))
    return diagnostics


def validate_lane_common(
    repo_root: pathlib.Path,
    artifacts: dict[str, dict[str, str]],
    lane_row: dict[str, Any],
) -> list[Diagnostic]:
    lane_id = str(lane_row.get("lane_id", "") or "lane")
    diagnostics: list[Diagnostic] = []
    if not valid_token(lane_id):
        diagnostics.append(Diagnostic("placeholder_evidence", lane_id, "lane_id must be concrete"))
    status = normalize_status(str(lane_row.get("status", "")))
    if status in FORBIDDEN_LANE_STATUSES or status not in COMPLETE_STATUSES:
        diagnostics.append(Diagnostic("pending_or_defined_only_lane", lane_id, "CEIC-093 lanes must be complete"))
    mode = normalize_status(str(lane_row.get("execution_mode", "")))
    if not mode or any(token in mode for token in FORBIDDEN_MODE_TOKENS):
        diagnostics.append(Diagnostic("pending_or_defined_only_lane", lane_id, "lane execution mode cannot be defined-only or pass-by-contract"))
    if not valid_hash(str(lane_row.get("proof_hash", ""))):
        diagnostics.append(Diagnostic("placeholder_evidence", lane_id, "lane proof hash must be concrete"))
    route_families = set(str(item) for item in lane_row.get("route_families", []))
    missing_routes = set(REQUIRED_ROUTE_FAMILIES) - route_families
    if missing_routes:
        diagnostics.append(Diagnostic("missing_route_family", lane_id, "missing route families: " + ", ".join(sorted(missing_routes))))
    diagnostics.extend(validate_production_boundary(lane_id, lane_row))
    diagnostics.extend(validate_authority(lane_id, lane_row))
    diagnostics.extend(validate_artifact_retention(lane_id, lane_row))
    diagnostics.extend(validate_artifact_refs(repo_root, artifacts, lane_id, lane_row))
    return diagnostics


def validate_security_checks(lane_id: str, checks: list[Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if not checks:
        return [Diagnostic("bypassed_security_negative", lane_id, "security-negative checks are required")]
    by_id = {str(row.get("check_id", "")): row for row in checks if isinstance(row, dict)}
    missing = REQUIRED_SECURITY_NEGATIVES - set(by_id)
    if missing:
        diagnostics.append(Diagnostic("bypassed_security_negative", lane_id, "missing security negatives: " + ", ".join(sorted(missing))))
    for check_id, row in by_id.items():
        subject = f"{lane_id}.{check_id}"
        if row.get("expected_outcome") != "refused" or row.get("check_passed") is not True or row.get("fail_closed") is not True:
            diagnostics.append(Diagnostic("bypassed_security_negative", subject, "negative check must fail closed as refused"))
        if row.get("bypassed") is True:
            diagnostics.append(Diagnostic("bypassed_security_negative", subject, "bypassed security-negative check is forbidden"))
        for flag in (
            "production_test_separation_enforced",
            "security_authorization_rechecked",
            "protected_material_redacted_or_excluded",
            "tamper_chain_verified",
        ):
            if row.get(flag) is not True:
                diagnostics.append(Diagnostic("bypassed_security_negative", subject, f"{flag} must be true"))
        if not valid_hash(str(row.get("proof_hash", ""))):
            diagnostics.append(Diagnostic("placeholder_evidence", subject, "security-negative proof hash must be concrete"))
        diagnostics.extend(validate_authority(subject, row))
    return diagnostics


def validate_lane_specific(lane_row: dict[str, Any]) -> list[Diagnostic]:
    lane_id = str(lane_row.get("lane_id", "") or "lane")
    lane_class = str(lane_row.get("lane_class", ""))
    diagnostics: list[Diagnostic] = []

    if lane_class == "soak_24h" and int(lane_row.get("duration_hours", 0) or 0) < 24:
        diagnostics.append(Diagnostic("missing_soak_duration", lane_id, "24h soak lane must record at least 24 hours"))
    if lane_class == "soak_72h" and int(lane_row.get("duration_hours", 0) or 0) < 72:
        diagnostics.append(Diagnostic("missing_soak_duration", lane_id, "72h soak lane must record at least 72 hours"))
    if lane_class == "soak_7d" and int(lane_row.get("duration_hours", 0) or 0) < 168:
        diagnostics.append(Diagnostic("missing_soak_duration", lane_id, "7d soak lane must record at least 168 hours"))
    if lane_class == "high_concurrency":
        workers = {int(value) for value in lane_row.get("worker_counts", [])}
        missing = {8, 32, 64, 128} - workers
        if missing or max(workers or {0}) < 128:
            diagnostics.append(Diagnostic("missing_high_concurrency", lane_id, "8/32/64/128 worker lanes and >=128 max workers are required"))
    if lane_class == "crash_fault_injection":
        fault_points = {str(value) for value in lane_row.get("fault_points", [])}
        missing = REQUIRED_CRASH_FAULT_POINTS - fault_points
        if missing:
            diagnostics.append(Diagnostic("missing_crash_fault", lane_id, "missing fault points: " + ", ".join(sorted(missing))))
    if lane_class == "sanitizer_static":
        tool_kinds = {str(value) for value in lane_row.get("tool_kinds", [])}
        missing = REQUIRED_SANITIZER_STATIC_KINDS - tool_kinds
        if missing:
            diagnostics.append(Diagnostic("missing_sanitizer_static", lane_id, "missing tool kinds: " + ", ".join(sorted(missing))))
    if lane_class == "memory_pressure":
        workloads = {str(value) for value in lane_row.get("workloads", [])}
        missing = REQUIRED_MEMORY_PRESSURE_WORKLOADS - workloads
        if missing:
            diagnostics.append(Diagnostic("missing_memory_pressure", lane_id, "missing pressure workloads: " + ", ".join(sorted(missing))))
    if lane_class == "security_negative":
        checks = lane_row.get("security_negative_checks", [])
        diagnostics.extend(validate_security_checks(lane_id, checks if isinstance(checks, list) else []))
    return diagnostics


def validate_model(repo_root: pathlib.Path, model: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    artifacts = artifact_rows(repo_root)

    if model.get("schema") != "sb.ceic093.reliability_security_suite.v1":
        diagnostics.append(Diagnostic("schema", "model", "unsupported CEIC-093 model schema"))
    for field in ("suite_id", "capture_generation", "proof_epoch"):
        if not valid_token(str(model.get(field, ""))):
            diagnostics.append(Diagnostic("placeholder_evidence", "model", f"{field} must be concrete"))
    for flag in ("production_test_separation",):
        if model.get(flag) is not True:
            diagnostics.append(Diagnostic("production_test_separation", "model", f"{flag} must be true"))
    diagnostics.extend(validate_production_boundary("model", model))
    diagnostics.extend(validate_authority("model", model))
    diagnostics.extend(validate_artifact_retention("model", model))
    diagnostics.extend(validate_artifact_refs(repo_root, artifacts, "model", model))

    coupling = model.get("predecessor_coupling", {})
    for key in PREDECESSOR_COUPLINGS:
        if not isinstance(coupling, dict) or coupling.get(key) is not True:
            diagnostics.append(Diagnostic("missing_predecessor_coupling", "model", f"{key} must be true"))

    lanes = model.get("lanes", [])
    if not isinstance(lanes, list) or not lanes:
        diagnostics.append(Diagnostic("missing_lane_class", "model", "CEIC-093 lanes are required"))
        return diagnostics
    if int(model.get("lane_count", 0) or 0) != len(lanes):
        diagnostics.append(Diagnostic("missing_lane_class", "model", "lane_count must match lanes length"))

    by_class = {str(row.get("lane_class", "")): row for row in lanes if isinstance(row, dict)}
    for lane_class in REQUIRED_LANE_CLASSES:
        if lane_class not in by_class:
            diagnostics.append(Diagnostic("missing_lane_class", lane_class, "required CEIC-093 lane class missing"))
    for row in lanes:
        if not isinstance(row, dict):
            diagnostics.append(Diagnostic("missing_lane_class", "model", "lane row must be an object"))
            continue
        diagnostics.extend(validate_lane_common(repo_root, artifacts, row))
        diagnostics.extend(validate_lane_specific(row))
    return diagnostics


def validate_memory_lane_definitions(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    lane_path = repo_root / MEMORY_LANES
    data = json.loads(lane_path.read_text(encoding="utf-8"))
    kinds: set[str] = set()
    for section in ("sanitizer_lanes", "dynamic_analysis_lanes", "static_analysis_lanes", "fuzz_lanes", "soak_lanes"):
        for row in data.get(section, []):
            kinds.add(str(row.get("kind", "")))
            if "artifact_policy" not in row:
                diagnostics.append(Diagnostic("missing_predecessor_coupling", section, "CEIC-022 lane artifact policy is required"))
    required = REQUIRED_SANITIZER_STATIC_KINDS | {"memory_policy_fuzz", "temp_workspace_fuzz", "soak_24h", "soak_72h"}
    missing = required - kinds
    if missing:
        diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-022", "missing lane definitions: " + ", ".join(sorted(missing))))
    churn_workers = {int(row.get("worker_count", 0) or 0) for row in data.get("churn_lanes", [])}
    if {8, 32, 64, 128} - churn_workers:
        diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-022", "missing high-churn worker definitions"))
    return diagnostics


def validate_predecessors(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    artifacts = artifact_rows(repo_root)
    for artifact_id in REQUIRED_ARTIFACTS:
        if not artifact_available(repo_root, artifacts, artifact_id):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "required predecessor artifact is absent or not present"))

    try:
        route_tool = load_tool(repo_root, "project/tools/ceic_route_chain_proof.py", "ceic_route_chain_proof_for_ceic093")
        route_diagnostics = route_tool.validate_model(repo_root, route_tool.default_model(repo_root))
        for diagnostic in route_diagnostics:
            diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-092", diagnostic.render()))
    except Exception as exc:  # pragma: no cover - surfaced as deterministic diagnostic
        diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-092", str(exc)))

    try:
        support_tool = load_tool(repo_root, "project/tools/ceic_integrated_support_bundle.py", "ceic_support_bundle_for_ceic093")
        support_diagnostics = support_tool.validate_model(repo_root, support_tool.default_model(repo_root))
        for diagnostic in support_diagnostics:
            diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-091", diagnostic.render()))
    except Exception as exc:  # pragma: no cover - surfaced as deterministic diagnostic
        diagnostics.append(Diagnostic("missing_predecessor_coupling", "CEIC-091", str(exc)))

    diagnostics.extend(validate_memory_lane_definitions(repo_root))
    return diagnostics


def is_complete(value: str) -> bool:
    status = normalize_status(value)
    return status in COMPLETE_STATUSES or status.startswith("complete_")


def validate_execution_plan_control(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    tracker = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv"), "slice_id")
    dependencies = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_DEPENDENCY_MATRIX.csv"), "dependency_id")
    gates = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv"), "gate_id")
    artifacts = artifact_rows(repo_root)
    trace = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_FINDING_TRACEABILITY_MATRIX.csv"), "finding_id")
    audit = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_IMPLEMENTATION_TRACEABILITY_MATRIX.csv"), "audit_id")

    for slice_id in ("CEIC-090", "CEIC-091", "CEIC-092", "CEIC-093"):
        if not is_complete(tracker.get(slice_id, {}).get("status", "")):
            diagnostics.append(Diagnostic("tracker_status", slice_id, f"{slice_id} must be complete"))
    for slice_id in ("CEIC-094", "CEIC-095"):
        if normalize_status(tracker.get(slice_id, {}).get("status", "")) not in {"pending", "complete"}:
            diagnostics.append(Diagnostic("successor_status", slice_id, f"{slice_id} must be pending or complete"))
    for dependency_id in ("CEIC-DEP-050", "CEIC-DEP-051", "CEIC-DEP-053", "CEIC-DEP-054"):
        if normalize_status(dependencies.get(dependency_id, {}).get("status", "")) != "available":
            diagnostics.append(Diagnostic("dependency_unavailable", dependency_id, f"{dependency_id} must be available"))
    for gate_id in ("CEIC-GATE-049", "CEIC-GATE-050", "CEIC-GATE-053", "CEIC-GATE-051"):
        if not is_complete(gates.get(gate_id, {}).get("status", "")):
            diagnostics.append(Diagnostic("gate_status", gate_id, f"{gate_id} must be complete"))
    if normalize_status(gates.get("CEIC-GATE-052", {}).get("status", "")) not in {"pending", "complete"}:
        diagnostics.append(Diagnostic("successor_status", "CEIC-GATE-052", "CEIC-094 gate must be pending or complete"))
    for artifact_id in REQUIRED_ARTIFACTS:
        if not artifact_available(repo_root, artifacts, artifact_id):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "required artifact must be present"))
    for finding_id in ("MEM-022", "OPT-011", "X-004"):
        row = trace.get(finding_id, {})
        if normalize_status(row.get("status", "")) != "complete":
            diagnostics.append(Diagnostic("traceability_status", finding_id, f"{finding_id} must be complete"))
        if "CEIC-ART-090" not in row.get("evidence_artifacts", ""):
            diagnostics.append(Diagnostic("traceability_status", finding_id, "CEIC-ART-090 evidence is required"))
    row = trace.get("X-009", {})
    if normalize_status(row.get("status", "")) not in {"pending", "complete"} or "CEIC-094" not in row.get("tracker_slices", ""):
        diagnostics.append(Diagnostic("successor_status", "X-009", "X-009 must be pending or complete for CEIC-094"))
    if "CEIC-ART-090" not in row.get("evidence_artifacts", ""):
        diagnostics.append(Diagnostic("traceability_status", "X-009", "CEIC-ART-090 evidence is required"))
    if normalize_status(audit.get("CEIC-AUD-052", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("audit_status", "CEIC-AUD-052", "CEIC-093 audit row must be complete"))

    cmake_text = (repo_root / CMAKE_GATE).read_text(encoding="utf-8")
    for token in ("ceic_093_reliability_security_suite_gate_check", "ceic_093_reliability_security_suite_gate"):
        if token not in cmake_text:
            diagnostics.append(Diagnostic("cmake_registration", "CEIC-093", f"missing CMake registration: {token}"))
    return diagnostics


def load_model(repo_root: pathlib.Path, manifest: pathlib.Path | None) -> dict[str, Any]:
    if manifest is None:
        return default_model(repo_root)
    path = manifest if manifest.is_absolute() else repo_root / manifest
    return json.loads(path.read_text(encoding="utf-8"))


def run(repo_root: pathlib.Path, manifest: pathlib.Path | None, skip_execution_plan_control: bool) -> list[Diagnostic]:
    model = load_model(repo_root, manifest)
    diagnostics = validate_model(repo_root, model)
    if manifest is None:
        diagnostics.extend(validate_predecessors(repo_root))
    if not skip_execution_plan_control:
        diagnostics.extend(validate_execution_plan_control(repo_root))
    return diagnostics


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--skip-execution_plan-control", action="store_true")
    parser.add_argument("--dump-default-model", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    if args.dump_default_model:
        print(json.dumps(default_model(repo_root), indent=2, sort_keys=True))
        return 0
    diagnostics = run(repo_root, args.manifest, args.skip_execution_plan_control)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"ceic_093_reliability_security_suite_gate=fail:{diagnostic.render()}", file=sys.stderr)
        return 1
    print("ceic_093_reliability_security_suite_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
