#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the CEIC-085 agent enterprise readiness manifest.

SEARCH_KEY: CEIC_085_AGENT_READINESS_MANIFEST_TOOL
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


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure")
DEFAULT_MANIFEST = EXECUTION_PLAN / "artifacts/CEIC-085_AGENT_READINESS_MANIFEST.yaml"
EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-085_AGENT_READINESS_MANIFEST_EVIDENCE.md"

COMPLETE_STATUS = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUS = {"present", "complete", "completed", "generated"}
PENDING_STATUS = {"pending", "planned"}

AGENT_INPUT_SLICES = tuple(f"CEIC-{value:03d}" for value in range(70, 85))
AGENT_MANIFEST_SLICES = AGENT_INPUT_SLICES + ("CEIC-085",)
PENDING_INTEGRATED_SLICES = tuple(f"CEIC-{value:03d}" for value in range(90, 96))

REQUIRED_COMPONENT_ARTIFACTS = tuple(f"CEIC-ART-{value:03d}" for value in range(71, 86))
REQUIRED_MANIFEST_ARTIFACTS = ("CEIC-ART-014", "CEIC-ART-086")
COUPLED_READINESS_ARTIFACTS = ("CEIC-ART-011", "CEIC-ART-012", "CEIC-ART-013")

AUTHORITY_BOUNDARY_TOKEN = (
    "agent_readiness_manifest_is_generated_evidence_only_not_runtime_authority_"
    "transaction_finality_visibility_authorization_security_recovery_parser_reference_"
    "wal_benchmark_optimizer_plan_index_finality_provider_finality_cluster_memory_"
    "or_agent_action_authority"
)

AGENT_PROFILE_ROWS = (
    ("node_resource_agent", "live_ready_noncluster", "foreground_guard"),
    ("storage_health_manager", "live_ready_noncluster", "storage_maintenance"),
    ("memory_governor", "live_ready_noncluster", "memory_pressure"),
    ("index_health_manager", "advisory_noncluster", "index_maintenance"),
    ("runtime_learning_agent", "advisory_noncluster", "optimizer_advisory"),
    ("support_bundle_triage_agent", "workflow_only_noncluster", "support_observability"),
    ("cluster_autoscale_manager", "external_provider_only_cluster", "fail_closed"),
)

LANE_ROWS = (
    "foreground_guard",
    "storage_maintenance",
    "memory_pressure",
    "optimizer_advisory",
    "index_maintenance",
    "backup_archive",
    "security_session",
    "support_observability",
    "low_priority_background",
)


@dataclass(frozen=True)
class SourceAnchor:
    slice_id: str
    path: str
    search_key: str
    surface: str


@dataclass(frozen=True)
class ComponentSpec:
    slice_id: str
    component_id: str
    title: str
    artifact_id: str
    ctest: str
    schema_id: str
    proof_kind: str
    anchors: tuple[SourceAnchor, ...]


COMPONENT_SPECS = (
    ComponentSpec(
        "CEIC-070",
        "agent_profiles",
        "AgentSystemProfile claim levels",
        "CEIC-ART-071",
        "agent_system_profile_claim_gate",
        "sb.agent.system_profile.v1",
        "profile_claim_level_validation",
        (
            SourceAnchor(
                "CEIC-070",
                "project/src/core/agents/agent_system_profile.hpp",
                "CEIC_070_AGENT_SYSTEM_PROFILE_DURABLE_CLAIM_LEVELS",
                "durable profile claim levels",
            ),
            SourceAnchor(
                "CEIC-070",
                "project/src/core/agents/agent_system_profile.cpp",
                "ValidateAgentSystemProfileClaim",
                "profile validation",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-071",
        "signed_typed_policies",
        "Signed typed policy lifecycle",
        "CEIC-ART-072",
        "agent_policy_lifecycle_gate",
        "sb.agent.policy_lifecycle.v1",
        "signed_typed_policy_validation",
        (
            SourceAnchor(
                "CEIC-071",
                "project/src/core/agents/agent_policy_lifecycle.hpp",
                "CEIC_071_SIGNED_TYPED_POLICY_LIFECYCLE",
                "signed lifecycle schema",
            ),
            SourceAnchor(
                "CEIC-071",
                "project/src/core/agents/agent_policy_lifecycle.cpp",
                "ValidateAgentSignedPolicyLifecycle",
                "signed lifecycle validation",
            ),
            SourceAnchor(
                "CEIC-071",
                "project/src/core/agents/agent_policy_schema.hpp",
                "ARHC_TYPED_POLICY_SCHEMAS_DEFAULTS",
                "typed policy schema coverage",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-072",
        "policy_override_resolution",
        "Policy inheritance and override resolution",
        "CEIC-ART-073",
        "agent_policy_override_resolution_gate",
        "sb.agent.policy_override_resolution.v1",
        "deterministic_override_resolution",
        (
            SourceAnchor(
                "CEIC-072",
                "project/src/core/agents/agent_policy_override_resolution.hpp",
                "CEIC_072_AGENT_POLICY_OVERRIDE_RESOLUTION",
                "policy override schema",
            ),
            SourceAnchor(
                "CEIC-072",
                "project/src/core/agents/agent_policy_override_resolution.cpp",
                "ResolveAgentPolicyOverrides",
                "deterministic override resolver",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-073",
        "lane_slo_cost_governance",
        "Execution lane SLO and cost governance",
        "CEIC-ART-074",
        "agent_execution_lane_governance_gate",
        "sb.agent.execution_lane_governance.v1",
        "lane_slo_cost_governance",
        (
            SourceAnchor(
                "CEIC-073",
                "project/src/core/agents/agent_execution_lane_governance.hpp",
                "CEIC_073_AGENT_EXECUTION_LANE_GOVERNANCE",
                "lane governance schema",
            ),
            SourceAnchor(
                "CEIC-073",
                "project/src/core/agents/agent_execution_lane_governance.cpp",
                "EvaluateAgentExecutionLaneAdmission",
                "lane admission validator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-074",
        "metric_quorum_source_attestation",
        "Strict metric quorum and source attestation",
        "CEIC-ART-075",
        "agent_metric_quorum_source_attestation_gate",
        "sb.agent.metric_quorum.v1",
        "metric_quorum_source_attestation",
        (
            SourceAnchor(
                "CEIC-074",
                "project/src/core/agents/agent_metric_runtime.hpp",
                "ARHC_STRICT_METRIC_SNAPSHOT_INTEGRATION",
                "strict metric snapshot inputs",
            ),
            SourceAnchor(
                "CEIC-074",
                "project/src/core/agents/agent_metric_runtime.cpp",
                "EvaluateAgentObservedMetricSnapshots",
                "observed metric quorum evaluator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-075",
        "action_safety_rollout",
        "Action safety envelopes and rollout profiles",
        "CEIC-ART-076",
        "agent_action_safety_rollout_gate",
        "sb.agent.action_safety_rollout.v1",
        "action_safety_rollout_validation",
        (
            SourceAnchor(
                "CEIC-075",
                "project/src/core/agents/agent_action_safety.hpp",
                "CEIC_075_AGENT_ACTION_SAFETY_ENVELOPE",
                "action safety envelope",
            ),
            SourceAnchor(
                "CEIC-075",
                "project/src/core/agents/agent_rollout_profile.hpp",
                "CEIC_075_AGENT_ROLLOUT_PROFILE",
                "rollout profile schema",
            ),
            SourceAnchor(
                "CEIC-075",
                "project/src/core/agents/agent_action_dispatch.cpp",
                "DispatchAgentAction",
                "dispatch preflight integration",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-076",
        "approval_break_glass",
        "Approval break-glass workflow",
        "CEIC-ART-077",
        "agent_approval_break_glass_workflow_gate",
        "sb.agent.manual_approval.v1",
        "approval_break_glass_validation",
        (
            SourceAnchor(
                "CEIC-076",
                "project/src/core/agents/agent_manual_approval.hpp",
                "CEIC_076_AGENT_MANUAL_APPROVAL_WORKFLOW",
                "manual approval schema",
            ),
            SourceAnchor(
                "CEIC-076",
                "project/src/core/agents/agent_manual_approval.cpp",
                "ValidateAgentManualApprovalWorkflow",
                "manual approval validation",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-077",
        "evidence_key_privacy_tamper_chain",
        "Evidence key privacy and tamper chain",
        "CEIC-ART-078",
        "agent_evidence_key_privacy_tamper_chain_gate",
        "sb.agent.evidence_tamper_chain.v1",
        "privacy_key_tamper_chain_validation",
        (
            SourceAnchor(
                "CEIC-077",
                "project/src/core/agents/agent_commercial_evidence.hpp",
                "ARHC_AGENT_EVIDENCE_REDACTION_RETENTION_TAMPER",
                "evidence redaction retention tamper schema",
            ),
            SourceAnchor(
                "CEIC-077",
                "project/src/core/agents/agent_commercial_evidence.cpp",
                "ValidateCommercialAgentEvidenceChain",
                "evidence chain validation",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-078",
        "dependency_lifecycle",
        "Dependency lifecycle DR restore clone behavior",
        "CEIC-ART-079",
        "agent_dependency_lifecycle_dr_restore_clone_gate",
        "sb.agent.dependency_lifecycle.v1",
        "dependency_lifecycle_validation",
        (
            SourceAnchor(
                "CEIC-078",
                "project/src/core/agents/agent_dependency_lifecycle.hpp",
                "CEIC_078_AGENT_DEPENDENCY_LIFECYCLE",
                "dependency lifecycle schema",
            ),
            SourceAnchor(
                "CEIC-078",
                "project/src/core/agents/agent_dependency_lifecycle.cpp",
                "EvaluateAgentDependencyLifecycle",
                "dependency lifecycle evaluator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-079",
        "tenant_coordination",
        "Tenant workload isolation and coordination",
        "CEIC-ART-080",
        "agent_tenant_coordination_gate",
        "sb.agent.tenant_coordination.v1",
        "tenant_coordination_validation",
        (
            SourceAnchor(
                "CEIC-079",
                "project/src/core/agents/agent_tenant_coordination.hpp",
                "CEIC_079_AGENT_TENANT_COORDINATION",
                "tenant coordination schema",
            ),
            SourceAnchor(
                "CEIC-079",
                "project/src/core/agents/agent_tenant_coordination.cpp",
                "EvaluateAgentTenantWorkloadCoordination",
                "tenant coordination evaluator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-080",
        "package_plugin_actuator_provenance",
        "Package plugin actuator provenance",
        "CEIC-ART-081",
        "agent_package_plugin_actuator_provenance_gate",
        "sb.agent.package_provenance.v1",
        "package_plugin_actuator_provenance",
        (
            SourceAnchor(
                "CEIC-080",
                "project/src/core/agents/agent_package_provenance.hpp",
                "CEIC_080_AGENT_PACKAGE_PLUGIN_ACTUATOR_PROVENANCE",
                "package provenance schema",
            ),
            SourceAnchor(
                "CEIC-080",
                "project/src/core/agents/agent_package_provenance.cpp",
                "ValidateAgentPackageProvenanceBundle",
                "package provenance validation",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-081",
        "replay_compensation_quarantine",
        "Replay compensation retry and quarantine",
        "CEIC-ART-082",
        "agent_replay_compensation_quarantine_gate",
        "sb.agent.replay_quarantine.v1",
        "replay_compensation_quarantine_validation",
        (
            SourceAnchor(
                "CEIC-081",
                "project/src/core/agents/agent_replay_quarantine.hpp",
                "CEIC_081_AGENT_REPLAY_QUARANTINE",
                "replay quarantine schema",
            ),
            SourceAnchor(
                "CEIC-081",
                "project/src/core/agents/agent_replay_quarantine.cpp",
                "ApplyAgentReplayControl",
                "replay control evaluator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-082",
        "noncluster_surface_closure",
        "Noncluster surface closure",
        "CEIC-ART-083",
        "agent_production_exposure_gate",
        "sb.agent.noncluster_surface_closure.v1",
        "no_anchor_only_live_surface_validation",
        (
            SourceAnchor(
                "CEIC-082",
                "project/src/core/agents/agent_production_classification.hpp",
                "ARHC_PER_AGENT_LIVE_RECOMMEND_DISABLED_CLASSIFICATION",
                "production classification schema",
            ),
            SourceAnchor(
                "CEIC-082",
                "project/src/core/agents/agent_production_classification.cpp",
                "ValidateAgentProductionExposureMatrix",
                "production exposure validator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-083",
        "memory_pressure_metric_integration",
        "Memory pressure and metric integration",
        "CEIC-ART-084",
        "agent_memory_pressure_metric_integration_gate",
        "sb.agent.memory_pressure_metric_integration.v1",
        "memory_pressure_metric_integration",
        (
            SourceAnchor(
                "CEIC-083",
                "project/src/core/agents/agent_memory_coupling.hpp",
                "CEIC_083_AGENT_MEMORY_PRESSURE_METRIC_INTEGRATION",
                "agent memory pressure schema",
            ),
            SourceAnchor(
                "CEIC-083",
                "project/src/core/agents/agent_memory_coupling.cpp",
                "EvaluateAgentMemoryPressureLaneIntegration",
                "memory pressure lane evaluator",
            ),
        ),
    ),
    ComponentSpec(
        "CEIC-084",
        "index_optimizer_boundary",
        "Agent index and optimizer integration boundary",
        "CEIC-ART-085",
        "agent_index_optimizer_boundary_gate",
        "sb.agent.index_optimizer_boundary.v1",
        "index_optimizer_boundary_validation",
        (
            SourceAnchor(
                "CEIC-084",
                "project/src/core/agents/agent_optimizer_recommendation.hpp",
                "CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY",
                "agent optimizer recommendation schema",
            ),
            SourceAnchor(
                "CEIC-084",
                "project/src/core/agents/agent_optimizer_recommendation.cpp",
                "EvaluateAgentIndexOptimizerBoundary",
                "index optimizer boundary evaluator",
            ),
            SourceAnchor(
                "CEIC-084",
                "project/src/engine/optimizer/agent_optimizer_recommendation_bridge.cpp",
                "CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY",
                "optimizer bridge handoff",
            ),
        ),
    ),
)

EXTRA_STATIC_ANCHORS = (
    SourceAnchor(
        "CEIC-085",
        "project/tools/ceic_agent_readiness_manifest.py",
        "CEIC_085_AGENT_READINESS_MANIFEST_TOOL",
        "agent readiness manifest producer",
    ),
    SourceAnchor(
        "CEIC-085",
        "project/tests/consolidated_enterprise/ceic_085_agent_readiness_manifest_gate_test.py",
        "CEIC_085_AGENT_READINESS_MANIFEST_GATE_TEST",
        "agent readiness manifest focused gate",
    ),
    SourceAnchor(
        "CEIC-085",
        "project/tests/agents/agent_security_crash_fixture_hardening_gate.cpp",
        "ARHC_SECURITY_NEGATIVE_BYPASS_TESTS",
        "agent crash/security negative fixture hardening",
    ),
    SourceAnchor(
        "CEIC-085",
        "project/tests/agents/agent_security_grant_matrix_gate.cpp",
        "support bundle contract did not require redaction right",
        "agent security grant matrix",
    ),
    SourceAnchor(
        "CEIC-085",
        "project/tests/agents/agent_zero_grey_output_contract_gate.cpp",
        "support bundle refused exact zero-grey evidence",
        "zero-grey support-bundle output contract",
    ),
    SourceAnchor(
        "CEIC-085",
        "project/tests/agents/agent_cluster_provider_boundary_gate.cpp",
        "external provider required",
        "cluster provider boundary fail-closed proof",
    ),
)

CONTROL_INPUTS = (
    EXECUTION_PLAN / "TRACKER.csv",
    EXECUTION_PLAN / "ARTIFACT_INDEX.csv",
    EXECUTION_PLAN / "ACCEPTANCE_GATES.csv",
    EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv",
    EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv",
    EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    EXECUTION_PLAN / "DEPENDENCIES.csv",
    EXECUTION_PLAN / "EVIDENCE_MANIFEST_SCHEMA.md",
    EXECUTION_PLAN / "README.md",
    EXECUTION_PLAN / "INTERFACE_CONTRACTS.md",
    pathlib.Path("project/tests/agents/CMakeLists.txt"),
    pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt"),
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
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


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
    return result.stdout.strip() or fallback


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


def artifact_exists(repo_root: pathlib.Path, row: dict[str, str]) -> bool:
    raw_path = row.get("path", "").strip()
    if not raw_path:
        return False
    if any(char in raw_path for char in "*?["):
        return any(repo_root.glob(raw_path))
    return (repo_root / raw_path).exists()


def artifact_digest(repo_root: pathlib.Path, row: dict[str, str]) -> str:
    raw_path = row.get("path", "").strip()
    if any(char in raw_path for char in "*?["):
        digest = hashlib.sha256()
        for match in sorted(repo_root.glob(raw_path)):
            rel = match.relative_to(repo_root).as_posix()
            digest.update(rel.encode("utf-8"))
            digest.update(b"\0")
            digest.update(sha256_file(match).encode("ascii"))
            digest.update(b"\0")
        return digest.hexdigest()
    return sha256_file(repo_root / raw_path)


def require_search_key(repo_root: pathlib.Path, rel: pathlib.Path, search_key: str) -> None:
    if search_key not in read_text(repo_root, rel):
        raise ManifestError(f"{rel} missing search key {search_key}")


def validate_execution_plan_inputs(
    repo_root: pathlib.Path,
    *,
    writing: bool,
) -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]], list[dict[str, str]]]:
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "TRACKER.csv"), "slice_id", "TRACKER.csv")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id", "ARTIFACT_INDEX.csv")
    metrics = read_csv(repo_root, EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv")

    for slice_id in AGENT_MANIFEST_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status not in COMPLETE_STATUS:
            raise ManifestError(f"{slice_id} must be complete before CEIC-085 manifest validation")

    for slice_id in PENDING_INTEGRATED_SLICES:
        if normalize_status(tracker.get(slice_id, {}).get("status", "")) not in PENDING_STATUS:
            raise ManifestError(f"{slice_id} must remain pending integrated proof in CEIC-085")

    manifest_row = artifacts.get("CEIC-ART-014")
    if manifest_row is None:
        raise ManifestError("CEIC-ART-014 agent readiness manifest artifact row missing")
    if manifest_row.get("path", "").strip() != DEFAULT_MANIFEST.as_posix():
        raise ManifestError("CEIC-ART-014 path must be the CEIC-085 agent readiness manifest")
    if normalize_status(manifest_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-014 must be present when CEIC-085 is complete")
    if not writing and not artifact_exists(repo_root, manifest_row):
        raise ManifestError("CEIC-ART-014 is present but the agent readiness manifest file is missing")

    evidence_row = artifacts.get("CEIC-ART-086")
    if evidence_row is None:
        raise ManifestError("CEIC-ART-086 agent readiness manifest evidence row missing")
    if evidence_row.get("path", "").strip() != EVIDENCE_MD.as_posix():
        raise ManifestError("CEIC-ART-086 path must be the CEIC-085 evidence markdown")

    for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:] + COUPLED_READINESS_ARTIFACTS:
        row = artifacts.get(artifact_id)
        if row is None:
            raise ManifestError(f"{artifact_id} required for CEIC-085 but missing from ARTIFACT_INDEX.csv")
        if normalize_status(row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError(f"{artifact_id} must be present before CEIC-085 manifest validation")
        if not artifact_exists(repo_root, row):
            raise ManifestError(f"{artifact_id} is marked present but missing: {row.get('path', '')}")

    agent_metrics = [
        row
        for row in metrics
        if "agent" in row.get("subsystem", "").lower()
        or row.get("metric_family", "").startswith("agent_")
    ]
    if not agent_metrics:
        raise ManifestError("METRICS_PRODUCER_COVERAGE_MATRIX.csv has no agent metric rows")
    for row in agent_metrics:
        if not row.get("required_producer_path", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no producer path")
        if not row.get("validation_gate", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no validation gate")

    return tracker, artifacts, agent_metrics


def validate_static_inputs(repo_root: pathlib.Path) -> None:
    for spec in COMPONENT_SPECS:
        for anchor in spec.anchors:
            require_search_key(repo_root, pathlib.Path(anchor.path), anchor.search_key)
    for anchor in EXTRA_STATIC_ANCHORS:
        require_search_key(repo_root, pathlib.Path(anchor.path), anchor.search_key)

    agent_cmake = read_text(repo_root, pathlib.Path("project/tests/agents/CMakeLists.txt"))
    for spec in COMPONENT_SPECS:
        if spec.ctest not in agent_cmake:
            raise ManifestError(f"project/tests/agents/CMakeLists.txt missing {spec.ctest}")
    for ctest in (
        "agent_security_crash_fixture_hardening_gate",
        "agent_security_grant_matrix_gate",
        "agent_zero_grey_output_contract_gate",
        "agent_cluster_provider_boundary_gate",
    ):
        if ctest not in agent_cmake:
            raise ManifestError(f"project/tests/agents/CMakeLists.txt missing {ctest}")

    consolidated_cmake = read_text(repo_root, pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt"))
    if "NAME ceic_085_agent_readiness_manifest_gate" not in consolidated_cmake:
        raise ManifestError("CMakeLists.txt missing add_test for ceic_085_agent_readiness_manifest_gate")
    if "ceic_085_agent_readiness_manifest_gate_check" not in consolidated_cmake:
        raise ManifestError("CMakeLists.txt missing ceic_085_agent_readiness_manifest_gate_check target")

    profile_gate = read_text(repo_root, pathlib.Path("project/tests/agents/agent_system_profile_claim_gate.cpp"))
    for token in (
        "stub-only live exposure",
        "local cluster live exposure",
        "external cluster provider proof",
        "cluster authority drift",
    ):
        if token not in profile_gate:
            raise ManifestError(f"agent profile gate missing {token}")

    production_gate = read_text(repo_root, pathlib.Path("project/tests/agents/agent_production_exposure_gate.cpp"))
    for token in (
        "anchor-only",
        "default proof set exposed live agents",
        "production",
        "ValidateAgentProductionExposureMatrix",
    ):
        if token not in production_gate:
            raise ManifestError(f"agent production exposure gate missing {token}")

    package_gate = read_text(
        repo_root,
        pathlib.Path("project/tests/agents/agent_package_plugin_actuator_provenance_gate.cpp"),
    )
    for token in (
        "CEIC-080 cluster package accepted without external provider proof",
        "external_cluster_provider_evidence_uuid",
        "forbidden_authority",
    ):
        if token not in package_gate:
            raise ManifestError(f"agent package provenance gate missing {token}")

    boundary_gate = read_text(repo_root, pathlib.Path("project/tests/agents/agent_index_optimizer_boundary_gate.cpp"))
    for token in (
        "cluster_without_provider",
        "external_provider_proof",
        "agent_index_optimizer_boundary.cluster_authority=false",
    ):
        if token not in boundary_gate:
            raise ManifestError(f"agent index/optimizer boundary gate missing {token}")


def collect_input_records(
    repo_root: pathlib.Path,
    artifacts: dict[str, dict[str, str]],
) -> tuple[list[dict[str, Any]], str]:
    rel_paths: set[pathlib.Path] = set(CONTROL_INPUTS)
    rel_paths.add(EVIDENCE_MD)
    for spec in COMPONENT_SPECS:
        rel_paths.add(pathlib.Path(f"project/tests/agents/{spec.ctest}.cpp"))
        for anchor in spec.anchors:
            rel_paths.add(pathlib.Path(anchor.path))
    for anchor in EXTRA_STATIC_ANCHORS:
        rel_paths.add(pathlib.Path(anchor.path))

    for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:] + COUPLED_READINESS_ARTIFACTS:
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
        records.append({"path": rel_text, "sha256": file_hash, "bytes": path.stat().st_size})
        digest.update(rel_text.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\0")

    return records, digest.hexdigest()


def non_placeholder_digest(label: str, source_digest: str) -> str:
    return "sha256:" + hashlib.sha256(f"ceic-085:{label}:{source_digest}".encode("utf-8")).hexdigest()


def build_component(
    repo_root: pathlib.Path,
    spec: ComponentSpec,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    source_digest: str,
) -> dict[str, Any]:
    row = artifacts[spec.artifact_id]
    component: dict[str, Any] = {
        "slice_id": spec.slice_id,
        "component_id": spec.component_id,
        "title": spec.title,
        "status": normalize_status(tracker[spec.slice_id]["status"]),
        "artifact_id": spec.artifact_id,
        "artifact_path": row["path"],
        "artifact_sha256": artifact_digest(repo_root, row),
        "artifact_state": "present",
        "ctest": spec.ctest,
        "ctest_registered": True,
        "schema_id": spec.schema_id,
        "proof_kind": spec.proof_kind,
        "source_anchors": [
            {
                "path": anchor.path,
                "search_key": anchor.search_key,
                "surface": anchor.surface,
                "sha256": sha256_file(repo_root / anchor.path),
            }
            for anchor in spec.anchors
        ],
        "evidence_contract_hash": non_placeholder_digest(f"{spec.component_id}:contract", source_digest),
        "evidence_digest": non_placeholder_digest(f"{spec.component_id}:evidence", source_digest),
        "static_only_readiness": False,
        "descriptor_only_readiness": False,
        "anchor_only_live_exposure": False,
        "fixture_test_only_production_path": False,
        "sidecar_only_evidence": False,
        "generated_readiness_runtime_authority": False,
        "production_live_claim": "component_evidence_only_no_enterprise_runtime_authority",
        "cluster_mode": "compile_time_stubbed_external_provider_only",
        "external_cluster_provider_proof_present": False,
        "cluster_claim_blocked": True,
        "cluster_production_claim": "fail_closed_missing_external_provider_proof",
        "transaction_finality_authority_claimed": False,
        "visibility_authority_claimed": False,
        "authorization_security_authority_claimed": False,
        "recovery_authority_claimed": False,
        "parser_authority_claimed": False,
        "reference_authority_claimed": False,
        "wal_authority_claimed": False,
        "benchmark_authority_claimed": False,
        "optimizer_plan_authority_claimed": False,
        "index_finality_authority_claimed": False,
        "provider_finality_authority_claimed": False,
        "memory_authority_claimed": False,
        "cluster_authority_claimed": False,
        "agent_action_authority_claimed": False,
    }

    if spec.component_id == "agent_profiles":
        component["claim_levels"] = [
            "disabled",
            "dry_run",
            "advisory",
            "live_ready_noncluster",
            "external_provider_only_cluster",
        ]
        component["profiles"] = [
            {"agent_type": agent_type, "claim_level": claim_level, "lane": lane}
            for agent_type, claim_level, lane in AGENT_PROFILE_ROWS
        ]
    elif spec.component_id == "signed_typed_policies":
        component["policy_layers"] = ["root", "database", "filespace", "tenant", "application", "session"]
        component["requires_digest_signature"] = True
        component["requires_author_approver"] = True
        component["requires_key_policy"] = True
    elif spec.component_id == "policy_override_resolution":
        component["scope_order"] = ["root", "database", "filespace", "tenant", "application", "session"]
        component["requires_approved_field_lists"] = True
        component["conflict_trace_required"] = True
    elif spec.component_id == "lane_slo_cost_governance":
        component["lanes"] = list(LANE_ROWS)
        component["slo_windows_required"] = True
        component["cost_center_chargeback_required"] = True
    elif spec.component_id == "metric_quorum_source_attestation":
        component["quorum_sources_minimum"] = 2
        component["source_attestation_required"] = True
        component["descriptor_only_metrics_admitted"] = False
    elif spec.component_id == "action_safety_rollout":
        component["rollout_modes"] = ["disabled", "shadow", "observe", "dry_run", "canary", "phased", "live"]
        component["provider_safety_required"] = True
        component["backup_checkpoint_required_for_live"] = True
    elif spec.component_id == "approval_break_glass":
        component["two_person_separation_required"] = True
        component["break_glass_review_deadline_required"] = True
        component["notification_escalation_required"] = True
    elif spec.component_id == "evidence_key_privacy_tamper_chain":
        component["redaction_before_buffering"] = True
        component["hmac_signature_required"] = True
        component["protected_material_suppressed"] = True
    elif spec.component_id == "dependency_lifecycle":
        component["modes"] = [
            "read_only",
            "restricted_open",
            "maintenance",
            "repair",
            "backup_restore",
            "archive_hold",
            "shutdown",
            "crash_recovery",
            "pitr",
            "clone",
            "role_change",
        ]
        component["generation_gap_refusal_required"] = True
    elif spec.component_id == "tenant_coordination":
        component["tenant_budget_required"] = True
        component["leader_follower_evidence_required"] = True
        component["foreground_protection_required"] = True
    elif spec.component_id == "package_plugin_actuator_provenance":
        component["subjects"] = ["plugin", "actuator_provider", "agent_binary"]
        component["sbom_required"] = True
        component["revocation_check_required"] = True
    elif spec.component_id == "replay_compensation_quarantine":
        component["digest_capture_inputs"] = [
            "policy",
            "metric",
            "catalog",
            "security",
            "resource",
            "binary_package",
            "action",
            "evidence_chain",
        ]
        component["quarantine_release_review_required"] = True
    elif spec.component_id == "noncluster_surface_closure":
        component["live_surface_classifications"] = [
            "live_noncluster",
            "workflow_only",
            "recommendation_only",
            "dry_run_only",
            "disabled",
            "external_provider_only_cluster",
        ]
        component["anchor_only_live_surface_admitted"] = False
    elif spec.component_id == "memory_pressure_metric_integration":
        component["required_coupling"] = ["CEIC-017", "CEIC-073", "CEIC-074"]
        component["decisions"] = ["allow", "throttle", "cancel", "shed_defer", "force_spill_cleanup"]
        component["memory_authority_claimed"] = False
    elif spec.component_id == "index_optimizer_boundary":
        component["required_coupling"] = ["CEIC-042", "CEIC-062"]
        component["recommendation_only"] = True
        component["optimizer_feedback_advisory_only"] = True

    return component


def build_manifest(
    repo_root: pathlib.Path,
    *,
    writing: bool = False,
    carried: dict[str, str] | None = None,
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    carried = carried or {}
    tracker, artifacts, agent_metrics = validate_execution_plan_inputs(repo_root, writing=writing)
    validate_static_inputs(repo_root)
    input_records, input_digest = collect_input_records(repo_root, artifacts)

    source_commit = carried.get("source_commit") or git_value(repo_root, ["rev-parse", "HEAD"], "unknown")
    source_branch = carried.get("source_branch") or git_value(
        repo_root, ["rev-parse", "--abbrev-ref", "HEAD"], "unknown"
    )
    generated_at = carried.get("generated_at_utc") or git_value(
        repo_root, ["show", "-s", "--format=%cI", "HEAD"], "1970-01-01T00:00:00+00:00"
    )
    manifest_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"scratchbird.ceic.085.agents:{input_digest}"))

    components = [
        build_component(repo_root, spec, tracker, artifacts, input_digest)
        for spec in COMPONENT_SPECS
    ]
    pending_integrated = [
        {
            "slice_id": slice_id,
            "status": normalize_status(tracker[slice_id]["status"]),
            "reason": "integrated proof is outside CEIC-085 and must remain pending",
        }
        for slice_id in PENDING_INTEGRATED_SLICES
    ]
    artifact_evidence = [
        {
            "artifact_id": artifact_id,
            "slice_id": artifacts[artifact_id]["slice_id"],
            "kind": artifacts[artifact_id]["artifact_kind"],
            "path": artifacts[artifact_id]["path"],
            "sha256": artifact_digest(repo_root, artifacts[artifact_id]),
            "status": normalize_status(artifacts[artifact_id]["status"]),
        }
        for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:] + COUPLED_READINESS_ARTIFACTS
    ]
    metric_rows = [
        {
            "metric_family": row.get("metric_family", ""),
            "subsystem": row.get("subsystem", ""),
            "required_producer_path": row.get("required_producer_path", ""),
            "operation_path": row.get("operation_path", ""),
            "support_bundle_path": row.get("support_bundle_path", ""),
            "validation_gate": row.get("validation_gate", ""),
            "status": row.get("status", ""),
        }
        for row in agent_metrics
    ]
    coupled_manifests = [
        {
            "subsystem": "memory",
            "artifact_id": "CEIC-ART-011",
            "path": artifacts["CEIC-ART-011"]["path"],
            "sha256": artifact_digest(repo_root, artifacts["CEIC-ART-011"]),
            "coupling_status": "consumed_evidence_only",
        },
        {
            "subsystem": "index",
            "artifact_id": "CEIC-ART-012",
            "path": artifacts["CEIC-ART-012"]["path"],
            "sha256": artifact_digest(repo_root, artifacts["CEIC-ART-012"]),
            "coupling_status": "consumed_evidence_only",
        },
        {
            "subsystem": "optimizer",
            "artifact_id": "CEIC-ART-013",
            "path": artifacts["CEIC-ART-013"]["path"],
            "sha256": artifact_digest(repo_root, artifacts["CEIC-ART-013"]),
            "coupling_status": "consumed_evidence_only",
        },
    ]

    return {
        "schema_id": "scratchbird.ceic.evidence_manifest",
        "schema_version": 1,
        "manifest_kind": "agent_enterprise_readiness_manifest",
        "search_key": "CEIC_085_AGENT_READINESS_MANIFEST",
        "subsystem": "agents",
        "slice_ids": list(AGENT_MANIFEST_SLICES),
        "manifest_uuid": manifest_uuid,
        "source_commit": source_commit,
        "source_branch": source_branch,
        "generated_at_utc": generated_at,
        "generated_by": "project/tools/ceic_agent_readiness_manifest.py#CEIC_085_AGENT_READINESS_MANIFEST_TOOL",
        "source_evidence_digest": input_digest,
        "auditor": {
            "slice_id": "CEIC-085",
            "timestamp_utc": generated_at,
            "proof_state": "generated_and_validated",
        },
        "authority_boundary": {
            "boundary_token": AUTHORITY_BOUNDARY_TOKEN,
            "runtime_authority": False,
            "transaction_finality_authority": False,
            "visibility_authority": False,
            "authorization_security_authority": False,
            "security_authority": False,
            "recovery_authority": False,
            "parser_authority": False,
            "reference_authority": False,
            "wal_authority": False,
            "benchmark_authority": False,
            "optimizer_plan_authority": False,
            "index_finality_authority": False,
            "provider_finality_authority": False,
            "memory_authority": False,
            "cluster_authority": False,
            "agent_action_authority": False,
        },
        "readiness_state": {
            "agent_manifest": "generated",
            "docs_alone_runtime_proof": False,
            "static_only_readiness": False,
            "descriptor_only_readiness": False,
            "anchor_only_live_exposure": False,
            "generated_readiness_runtime_authority": False,
            "production_live_claim": "blocked_pending_integrated_proof",
            "enterprise_agent_readiness": "component_complete_integrated_proof_pending",
            "integrated_readiness": "pending",
            "support_bundle_readiness": "component_evidence_present_integrated_CEIC-091_pending",
            "cluster_production_claim": "fail_closed_missing_external_provider_proof",
            "cluster_readiness_claim": "fail_closed_missing_external_provider_proof",
            "pending_integrated_proof": pending_integrated,
        },
        "cluster_boundary": {
            "cluster_agent_code_state": "compile_time_stubbed_external_provider_only",
            "local_cluster_readiness": "fail_closed",
            "external_cluster_provider_only": True,
            "external_cluster_provider_proof_present": False,
            "external_cluster_provider_proof_id": "",
            "cluster_production_claim": "fail_closed_missing_external_provider_proof",
            "cluster_readiness_claim": "fail_closed_missing_external_provider_proof",
            "cluster_stub_claimed_as_production": False,
            "local_cluster_evidence_present": False,
            "cluster_runtime_authority": False,
        },
        "production_test_separation": {
            "fixture_test_only_production_paths": False,
            "test_keys_in_production": False,
            "synthetic_metric_quorum": False,
            "placeholder_evidence_contracts": False,
            "sidecar_only_evidence": False,
            "anchor_only_live_exposure": False,
            "cluster_stub_enabled_for_production": False,
        },
        "readiness_proofs": {
            "agent_profiles": {
                "component_id": "agent_profiles",
                "profiles": [
                    {"agent_type": agent_type, "claim_level": claim_level, "lane": lane}
                    for agent_type, claim_level, lane in AGENT_PROFILE_ROWS
                ],
            },
            "policies": {
                "component_ids": ["signed_typed_policies", "policy_override_resolution"],
                "signed_typed_policy_lifecycle": "complete",
                "override_resolution": "complete",
            },
            "lane_slo_proof": {
                "component_id": "lane_slo_cost_governance",
                "lanes": list(LANE_ROWS),
                "slo_cost_governance": "complete",
            },
            "metrics_proof": {
                "component_id": "metric_quorum_source_attestation",
                "state": "producer_rows_indexed_CEIC-090_integrated_coverage_pending",
                "rows": metric_rows,
            },
            "action_approval_proof": {
                "component_ids": ["action_safety_rollout", "approval_break_glass"],
                "safety_rollout": "complete",
                "approval_break_glass": "complete",
            },
            "evidence_proof": {
                "component_ids": ["evidence_key_privacy_tamper_chain", "replay_compensation_quarantine"],
                "privacy_tamper_chain": "complete",
                "replay_quarantine": "complete",
            },
            "plugin_provenance_proof": {
                "component_id": "package_plugin_actuator_provenance",
                "package_plugin_actuator_provenance": "complete",
            },
            "memory_coupling_proof": {
                "component_id": "memory_pressure_metric_integration",
                "memory_manifest": "CEIC-ART-011",
                "agent_memory_pressure_integration": "complete",
            },
            "index_optimizer_coupling_proof": {
                "component_id": "index_optimizer_boundary",
                "index_manifest": "CEIC-ART-012",
                "optimizer_manifest": "CEIC-ART-013",
                "recommendation_only": True,
            },
            "crash_security_proof": {
                "security_grant_gate": "agent_security_grant_matrix_gate",
                "crash_fixture_hardening_gate": "agent_security_crash_fixture_hardening_gate",
                "status": "focused_agent_gates_present_CEIC-093_integrated_suite_pending",
            },
            "support_bundle_readiness": {
                "zero_grey_gate": "agent_zero_grey_output_contract_gate",
                "status": "bounded_redacted_agent_rows_present_integrated_CEIC-091_pending",
                "integrated_support_bundle_ready": False,
            },
        },
        "components": components,
        "subsystem_coupling_manifests": coupled_manifests,
        "artifact_evidence": artifact_evidence,
        "build": {
            "cmake_file": "project/tests/consolidated_enterprise/CMakeLists.txt",
            "focused_ctest": "ceic_085_agent_readiness_manifest_gate",
            "focused_target": "ceic_085_agent_readiness_manifest_gate_check",
            "component_ctests": [spec.ctest for spec in COMPONENT_SPECS],
            "agent_nearby_ctests": [
                "agent_security_crash_fixture_hardening_gate",
                "agent_security_grant_matrix_gate",
                "agent_zero_grey_output_contract_gate",
                "agent_cluster_provider_boundary_gate",
            ],
            "state": "CMake-visible focused gates defined",
        },
        "input_records": input_records,
    }


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"{path} is not deterministic JSON/YAML-subset manifest: {exc}") from exc
    if not isinstance(data, dict):
        raise ManifestError(f"{path} must contain a JSON object")
    return data


def placeholder_value(value: Any) -> bool:
    text = str(value).lower()
    return (
        not text
        or text == "result-contract-v1"
        or text == "sha256:result-contract-v1"
        or text == "sha256:placeholder"
        or text == "placeholder"
        or "placeholder" in text
    )


def validate_component_semantics(component: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    component_id = str(component.get("component_id", "<missing>"))
    if component.get("status") not in COMPLETE_STATUS:
        errors.append(f"component {component_id} must be complete")
    if component.get("artifact_state") != "present":
        errors.append(f"component {component_id} artifact must be present")
    if component.get("ctest_registered") is not True:
        errors.append(f"component {component_id} focused CTest must be registered")
    if placeholder_value(component.get("evidence_contract_hash")):
        errors.append(f"component {component_id} placeholder evidence contract is forbidden")
    if component.get("static_only_readiness") is not False:
        errors.append(f"component {component_id} static-only readiness is forbidden")
    if component.get("descriptor_only_readiness") is not False:
        errors.append(f"component {component_id} descriptor-only readiness is forbidden")
    if component.get("anchor_only_live_exposure") is not False:
        errors.append(f"component {component_id} anchor-only live exposure is forbidden")
    if component.get("fixture_test_only_production_path") is not False:
        errors.append(f"component {component_id} fixture/test-only production path is forbidden")
    if component.get("sidecar_only_evidence") is not False:
        errors.append(f"component {component_id} sidecar-only evidence is forbidden")
    if component.get("generated_readiness_runtime_authority") is not False:
        errors.append(f"component {component_id} generated readiness cannot be runtime authority")
    if component.get("cluster_claim_blocked") is not True:
        errors.append(f"component {component_id} cluster claims must fail closed")
    if component.get("external_cluster_provider_proof_present") is not False:
        errors.append(f"component {component_id} current manifest must not claim external provider proof")
    if "fail_closed" not in str(component.get("cluster_production_claim", "")):
        errors.append(f"component {component_id} cluster production claim must fail closed")
    for field in (
        "transaction_finality_authority_claimed",
        "visibility_authority_claimed",
        "authorization_security_authority_claimed",
        "recovery_authority_claimed",
        "parser_authority_claimed",
        "reference_authority_claimed",
        "wal_authority_claimed",
        "benchmark_authority_claimed",
        "optimizer_plan_authority_claimed",
        "index_finality_authority_claimed",
        "provider_finality_authority_claimed",
        "memory_authority_claimed",
        "cluster_authority_claimed",
        "agent_action_authority_claimed",
    ):
        if component.get(field) is not False:
            errors.append(f"component {component_id} unsafe authority field is forbidden: {field}")
    return errors


def validate_manifest_semantics(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema_id") != "scratchbird.ceic.evidence_manifest":
        errors.append("schema_id mismatch")
    if data.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if data.get("manifest_kind") != "agent_enterprise_readiness_manifest":
        errors.append("manifest_kind mismatch")
    if data.get("search_key") != "CEIC_085_AGENT_READINESS_MANIFEST":
        errors.append("search_key mismatch")
    if data.get("slice_ids") != list(AGENT_MANIFEST_SLICES):
        errors.append("slice_ids must be CEIC-070..CEIC-085")
    for field in ("source_commit", "source_branch", "source_evidence_digest", "generated_at_utc", "generated_by"):
        if not data.get(field):
            errors.append(f"{field} is required")

    boundary = data.get("authority_boundary", {})
    if boundary.get("boundary_token") != AUTHORITY_BOUNDARY_TOKEN:
        errors.append("authority boundary token mismatch")
    for key, value in boundary.items():
        if key.endswith("_authority") and value is not False:
            errors.append(f"{key} must be false")
    if boundary.get("runtime_authority") is not False:
        errors.append("runtime_authority must be false")

    readiness = data.get("readiness_state", {})
    if readiness.get("docs_alone_runtime_proof") is not False:
        errors.append("docs-alone runtime proof is forbidden")
    if readiness.get("static_only_readiness") is not False:
        errors.append("static-only readiness is forbidden")
    if readiness.get("descriptor_only_readiness") is not False:
        errors.append("descriptor-only readiness is forbidden")
    if readiness.get("anchor_only_live_exposure") is not False:
        errors.append("anchor-only live exposure is forbidden")
    if readiness.get("generated_readiness_runtime_authority") is not False:
        errors.append("generated readiness cannot be runtime authority")
    if str(readiness.get("production_live_claim", "")).startswith("blocked") is not True:
        errors.append("production live claim must remain blocked pending integrated proof")
    if readiness.get("integrated_readiness") != "pending":
        errors.append("integrated readiness must remain pending")
    if "pending" not in str(readiness.get("support_bundle_readiness", "")):
        errors.append("support-bundle readiness must keep CEIC-091 pending")
    if "fail_closed" not in str(readiness.get("cluster_production_claim", "")):
        errors.append("cluster production claim must fail closed")
    if "fail_closed" not in str(readiness.get("cluster_readiness_claim", "")):
        errors.append("cluster readiness claim must fail closed")

    pending = {
        row.get("slice_id"): row.get("status")
        for row in readiness.get("pending_integrated_proof", [])
        if isinstance(row, dict)
    }
    for slice_id in PENDING_INTEGRATED_SLICES:
        if pending.get(slice_id) not in PENDING_STATUS:
            errors.append(f"{slice_id} must remain pending integrated proof")

    cluster = data.get("cluster_boundary", {})
    if cluster.get("cluster_agent_code_state") != "compile_time_stubbed_external_provider_only":
        errors.append("cluster agents/code must remain compile-time stubbed external-provider-only")
    if cluster.get("local_cluster_readiness") != "fail_closed":
        errors.append("local cluster readiness must fail closed")
    if cluster.get("external_cluster_provider_only") is not True:
        errors.append("cluster readiness must remain external-provider-only")
    if cluster.get("external_cluster_provider_proof_present") is not False:
        errors.append("current CEIC-085 manifest must not claim external-cluster-provider proof")
    if cluster.get("cluster_stub_claimed_as_production") is not False:
        errors.append("cluster stubs must not be claimed as production")
    if cluster.get("local_cluster_evidence_present") is not False:
        errors.append("local cluster evidence is forbidden")
    if cluster.get("cluster_runtime_authority") is not False:
        errors.append("cluster runtime authority is forbidden")
    if (
        "fail_closed" not in str(cluster.get("cluster_production_claim", ""))
        and cluster.get("external_cluster_provider_proof_present") is not True
    ):
        errors.append("cluster production claim missing external-cluster-provider proof")
    if (
        "fail_closed" not in str(cluster.get("cluster_readiness_claim", ""))
        and cluster.get("external_cluster_provider_proof_present") is not True
    ):
        errors.append("cluster readiness claim missing external-cluster-provider proof")

    separation = data.get("production_test_separation", {})
    for field in (
        "fixture_test_only_production_paths",
        "test_keys_in_production",
        "synthetic_metric_quorum",
        "placeholder_evidence_contracts",
        "sidecar_only_evidence",
        "anchor_only_live_exposure",
        "cluster_stub_enabled_for_production",
    ):
        if separation.get(field) is not False:
            errors.append(f"fixture/test-only production path is forbidden: {field}")

    proofs = data.get("readiness_proofs", {})
    required_proofs = {
        "agent_profiles",
        "policies",
        "lane_slo_proof",
        "metrics_proof",
        "action_approval_proof",
        "evidence_proof",
        "plugin_provenance_proof",
        "memory_coupling_proof",
        "index_optimizer_coupling_proof",
        "crash_security_proof",
        "support_bundle_readiness",
    }
    missing_proofs = sorted(required_proofs - set(proofs))
    if missing_proofs:
        errors.append(f"readiness proof sections missing: {', '.join(missing_proofs)}")
    if proofs.get("index_optimizer_coupling_proof", {}).get("recommendation_only") is not True:
        errors.append("index/optimizer coupling must remain recommendation-only")
    if proofs.get("support_bundle_readiness", {}).get("integrated_support_bundle_ready") is not False:
        errors.append("support-bundle readiness must not close CEIC-091")

    components = data.get("components", [])
    if not isinstance(components, list):
        errors.append("components must be a list")
        components = []
    component_ids = {row.get("component_id") for row in components if isinstance(row, dict)}
    required_components = {spec.component_id for spec in COMPONENT_SPECS}
    missing_components = sorted(required_components - component_ids)
    if missing_components:
        errors.append(f"required component missing: {', '.join(missing_components)}")
    for component in components:
        if isinstance(component, dict):
            errors.extend(validate_component_semantics(component))

    coupled = {
        row.get("artifact_id"): row
        for row in data.get("subsystem_coupling_manifests", [])
        if isinstance(row, dict)
    }
    for artifact_id in COUPLED_READINESS_ARTIFACTS:
        if coupled.get(artifact_id, {}).get("coupling_status") != "consumed_evidence_only":
            errors.append(f"{artifact_id} coupling must be consumed as evidence only")

    return errors


def validate_manifest(repo_root: pathlib.Path, manifest_path: pathlib.Path) -> list[str]:
    try:
        committed = load_manifest(manifest_path)
    except ManifestError as exc:
        return [str(exc)]

    carried = {
        key: str(committed.get(key, ""))
        for key in ("source_commit", "source_branch", "generated_at_utc")
        if committed.get(key)
    }
    try:
        generated = build_manifest(repo_root, carried=carried)
    except ManifestError as exc:
        return [str(exc)]

    errors = validate_manifest_semantics(committed)
    if committed != generated:
        errors.append("stale manifest differs from generated CEIC-085 proof")
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path.cwd(),
        help="ScratchBird" "-Private repository root",
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=DEFAULT_MANIFEST,
        help="Manifest path to write or validate",
    )
    parser.add_argument("--write", action="store_true", help="Write the generated manifest")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest
    if not manifest_path.is_absolute():
        manifest_path = repo_root / manifest_path

    if args.write:
        try:
            data = build_manifest(repo_root, writing=True)
        except ManifestError as exc:
            print(f"ceic_085_agent_readiness_manifest=fail:{exc}", file=sys.stderr)
            return 1
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(render_json(data), encoding="utf-8")
        print(f"ceic_085_agent_readiness_manifest=written:{manifest_path}")
        return 0

    errors = validate_manifest(repo_root, manifest_path)
    if errors:
        for error in errors:
            print(f"ceic_085_agent_readiness_manifest=fail:{error}", file=sys.stderr)
        return 1
    print("ceic_085_agent_readiness_manifest=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
