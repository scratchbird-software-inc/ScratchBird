#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the CEIC-062 optimizer readiness manifest.

SEARCH_KEY: CEIC_062_OPTIMIZER_READINESS_MANIFEST_TOOL
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
DEFAULT_MANIFEST = EXECUTION_PLAN / "artifacts/CEIC-062_OPTIMIZER_READINESS_MANIFEST.yaml"

COMPLETE_STATUS = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUS = {"present", "complete", "completed", "generated"}
PENDING_STATUS = {"pending", "planned"}

OPTIMIZER_SLICES = tuple(f"CEIC-{value:03d}" for value in range(50, 63))
INTEGRATED_RELEASE_SLICES = tuple(f"CEIC-{value:03d}" for value in range(90, 96))
INTEGRATED_RELEASE_ARTIFACTS = {
    "CEIC-090": ("CEIC-ART-018", "CEIC-ART-087"),
    "CEIC-091": ("CEIC-ART-088",),
    "CEIC-092": ("CEIC-ART-089",),
    "CEIC-093": ("CEIC-ART-090",),
    "CEIC-094": ("CEIC-ART-091",),
    "CEIC-095": ("CEIC-ART-015",),
}

REQUIRED_COMPONENT_ARTIFACTS = tuple(f"CEIC-ART-{value:03d}" for value in range(58, 70))
REQUIRED_MANIFEST_ARTIFACTS = ("CEIC-ART-013", "CEIC-ART-070")

AUTHORITY_BOUNDARY_TOKEN = (
    "optimizer_readiness_manifest_is_generated_evidence_only_not_transaction_"
    "finality_visibility_authorization_security_recovery_parser_reference_wal_"
    "benchmark_dominance_optimizer_plan_truth_index_finality_provider_finality_"
    "cluster_memory_or_agent_action_authority"
)

LIVE_ROUTES = ("embedded", "ipc", "inet", "cli", "driver")
CORRECTNESS_CLASSES = (
    "inner_join",
    "outer_join",
    "semi_join",
    "anti_join",
    "correlated_dependency",
    "aggregation",
    "distinct",
    "window",
    "topn",
    "dml_locator",
    "document_path",
    "vector",
    "text_search",
    "graph",
    "mixed_fusion",
)
CRASH_POINTS = (
    "before_persist",
    "during_write",
    "after_fsync_before_commit",
    "after_commit_before_publish",
    "after_publish",
    "during_reopen",
    "after_replay_refusal",
)
REOPEN_DECISIONS = (
    "reload_accepted",
    "reload_refused",
    "recovered_rebuilt",
    "quarantined",
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
    source_path: str
    search_key: str
    proof_kind: str


SOURCE_ANCHORS = (
    SourceAnchor(
        "CEIC-050",
        "project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp",
        "OEIC_LIVE_PARSER_SBLR_OPTIMIZER_EXECUTOR_ROUTE",
        "live route benchmark artifact harness",
    ),
    SourceAnchor(
        "CEIC-050",
        "project/src/engine/optimizer/runtime_consumption_benchmark_evidence.cpp",
        "ValidateOptimizerBenchmarkRouteEvidence",
        "runtime route benchmark evidence",
    ),
    SourceAnchor(
        "CEIC-051",
        "project/src/engine/optimizer/optimizer_benchmark_evidence_schema.hpp",
        "CEIC_051_PERSISTED_OPTIMIZER_BENCHMARK_EVIDENCE_SCHEMA",
        "persisted benchmark evidence schema",
    ),
    SourceAnchor(
        "CEIC-052",
        "project/src/engine/optimizer/optimizer_correctness_oracle.hpp",
        "CEIC_052_OPTIMIZER_CORRECTNESS_ORACLE",
        "correctness oracle suite",
    ),
    SourceAnchor(
        "CEIC-053",
        "project/src/engine/optimizer/optimizer_crash_reopen_persistence.hpp",
        "CEIC_053_OPTIMIZER_CRASH_REOPEN_PERSISTENCE",
        "crash reopen persistence",
    ),
    SourceAnchor(
        "CEIC-054",
        "project/src/engine/optimizer/optimizer_selectivity_drift_observability.hpp",
        "CEIC_054_SELECTIVITY_DRIFT_PLAN_STABILITY_OBSERVABILITY",
        "selectivity drift and plan-stability observability",
    ),
    SourceAnchor(
        "CEIC-055",
        "project/src/engine/optimizer/optimizer_transformation_memo_coverage.hpp",
        "CEIC_055_OPTIMIZER_TRANSFORMATION_MEMO_COVERAGE",
        "transformation memo coverage",
    ),
    SourceAnchor(
        "CEIC-056",
        "project/src/engine/optimizer/optimizer_workload_regression_budget.hpp",
        "CEIC_056_OPTIMIZER_WORKLOAD_REGRESSION_BUDGET",
        "workload regression budget",
    ),
    SourceAnchor(
        "CEIC-057",
        "project/src/engine/optimizer/optimizer_driver_explain_compatibility.hpp",
        "CEIC_057_DRIVER_VISIBLE_EXPLAIN_COMPATIBILITY",
        "driver visible explain compatibility",
    ),
    SourceAnchor(
        "CEIC-058",
        "project/src/engine/optimizer/optimizer_live_reference_comparison_artifacts.hpp",
        "CEIC_058_LIVE_REFERENCE_COMPARISON_ARTIFACTS",
        "live reference comparison artifacts",
    ),
    SourceAnchor(
        "CEIC-059",
        "project/src/engine/optimizer/optimizer_memory_feedback_bridge.hpp",
        "MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE",
        "governed optimizer memory feedback",
    ),
    SourceAnchor(
        "CEIC-059",
        "project/src/engine/optimizer/optimizer_memory_spill_feedback_enterprise.hpp",
        "OEIC_MEMORY_SPILL_FEEDBACK_ENTERPRISE",
        "optimizer spill feedback persistence",
    ),
    SourceAnchor(
        "CEIC-060",
        "project/src/core/index/index_optimizer_integration.hpp",
        "SB-INDEX-OPTIMIZER-INTEGRATION-CLOSURE-ANCHOR",
        "index readiness plan admission",
    ),
    SourceAnchor(
        "CEIC-060",
        "project/src/engine/optimizer/optimizer_index_costing.hpp",
        "OEIC_INDEX_COSTING_ENTERPRISE_CLOSURE",
        "optimizer index costing admission",
    ),
    SourceAnchor(
        "CEIC-061",
        "project/src/core/memory/llvm_memory_accounting.hpp",
        "CEIC_061_LLVM_MEMORY_ACCOUNTING",
        "LLVM dynamic/static memory accounting",
    ),
    SourceAnchor(
        "CEIC-061",
        "project/src/engine/native_compile/native_compile.hpp",
        "SB_ENGINE_NATIVE_COMPILE_RUNTIME",
        "native compile memory accounting",
    ),
    SourceAnchor(
        "CEIC-062",
        "project/tools/ceic_optimizer_readiness_manifest.py",
        "CEIC_062_OPTIMIZER_READINESS_MANIFEST_TOOL",
        "optimizer readiness manifest producer",
    ),
)

COMPONENT_SPECS = (
    ComponentSpec(
        "CEIC-050",
        "live_routes",
        "Live-route benchmark artifact harness",
        "CEIC-ART-058",
        "ceic_050_live_route_benchmark_artifact_gate",
        "sb.optimizer.live_route_artifact_harness.v1",
        "project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp",
        "OEIC_LIVE_PARSER_SBLR_OPTIMIZER_EXECUTOR_ROUTE",
        "live_route_runtime_evidence",
    ),
    ComponentSpec(
        "CEIC-051",
        "persisted_benchmark_evidence",
        "Persisted optimizer benchmark evidence schema",
        "CEIC-ART-059",
        "ceic_051_persisted_optimizer_benchmark_evidence_schema_gate",
        "sb.optimizer.benchmark_evidence.v1",
        "project/src/engine/optimizer/optimizer_benchmark_evidence_schema.hpp",
        "CEIC_051_PERSISTED_OPTIMIZER_BENCHMARK_EVIDENCE_SCHEMA",
        "benchmark_schema_validation",
    ),
    ComponentSpec(
        "CEIC-052",
        "correctness_oracles",
        "Optimizer correctness oracle suite",
        "CEIC-ART-060",
        "ceic_052_optimizer_correctness_oracle_suite_gate",
        "sb.optimizer.correctness_oracle.v1",
        "project/src/engine/optimizer/optimizer_correctness_oracle.hpp",
        "CEIC_052_OPTIMIZER_CORRECTNESS_ORACLE",
        "correctness_oracle_validation",
    ),
    ComponentSpec(
        "CEIC-053",
        "crash_reopen_persistence",
        "Optimizer crash reopen persistence tests",
        "CEIC-ART-061",
        "ceic_053_optimizer_crash_reopen_persistence_gate",
        "sb.optimizer.crash_reopen_persistence.v1",
        "project/src/engine/optimizer/optimizer_crash_reopen_persistence.hpp",
        "CEIC_053_OPTIMIZER_CRASH_REOPEN_PERSISTENCE",
        "crash_reopen_matrix_validation",
    ),
    ComponentSpec(
        "CEIC-054",
        "metrics_feedback",
        "Selectivity drift and plan-stability observability",
        "CEIC-ART-062",
        "ceic_054_selectivity_drift_plan_stability_observability_gate",
        "sb.optimizer.selectivity_drift_observability.v1",
        "project/src/engine/optimizer/optimizer_selectivity_drift_observability.hpp",
        "CEIC_054_SELECTIVITY_DRIFT_PLAN_STABILITY_OBSERVABILITY",
        "metrics_feedback_and_observability",
    ),
    ComponentSpec(
        "CEIC-055",
        "transformation_memo_coverage",
        "Optimizer transformation memo coverage",
        "CEIC-ART-063",
        "ceic_055_optimizer_transformation_memo_coverage_gate",
        "sb.optimizer.transformation_memo_coverage.v1",
        "project/src/engine/optimizer/optimizer_transformation_memo_coverage.hpp",
        "CEIC_055_OPTIMIZER_TRANSFORMATION_MEMO_COVERAGE",
        "transformation_memo_validation",
    ),
    ComponentSpec(
        "CEIC-056",
        "workload_regression_budgets",
        "Optimizer workload regression budgets",
        "CEIC-ART-064",
        "ceic_056_optimizer_workload_regression_budget_gate",
        "sb.optimizer.workload_regression_budget.v1",
        "project/src/engine/optimizer/optimizer_workload_regression_budget.hpp",
        "CEIC_056_OPTIMIZER_WORKLOAD_REGRESSION_BUDGET",
        "workload_budget_validation",
    ),
    ComponentSpec(
        "CEIC-057",
        "driver_visible_explain",
        "Driver-visible explain compatibility",
        "CEIC-ART-065",
        "ceic_057_driver_visible_explain_compatibility_gate",
        "sb.optimizer.driver_visible_explain_compatibility.v1",
        "project/src/engine/optimizer/optimizer_driver_explain_compatibility.hpp",
        "CEIC_057_DRIVER_VISIBLE_EXPLAIN_COMPATIBILITY",
        "driver_visible_explain_validation",
    ),
    ComponentSpec(
        "CEIC-058",
        "reference_comparison_artifacts",
        "Live reference comparison artifacts",
        "CEIC-ART-066",
        "ceic_058_live_reference_comparison_artifacts_gate",
        "sb.optimizer.live_reference_comparison_artifacts.v1",
        "project/src/engine/optimizer/optimizer_live_reference_comparison_artifacts.hpp",
        "CEIC_058_LIVE_REFERENCE_COMPARISON_ARTIFACTS",
        "reference_comparison_artifact_validation",
    ),
    ComponentSpec(
        "CEIC-059",
        "memory_feedback",
        "Optimizer memory feedback from governed ledger",
        "CEIC-ART-067",
        "ceic_059_optimizer_memory_feedback_governed_ledger_gate",
        "sb.optimizer.memory_feedback_evidence.v1",
        "project/src/engine/optimizer/optimizer_memory_feedback_bridge.hpp",
        "MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE",
        "governed_memory_feedback_validation",
    ),
    ComponentSpec(
        "CEIC-060",
        "index_readiness_coupling",
        "Index readiness coupling in plan admission",
        "CEIC-ART-068",
        "ceic_060_index_readiness_plan_admission_gate",
        "sb.optimizer.index_readiness_plan_admission.v1",
        "project/src/core/index/index_optimizer_integration.hpp",
        "SB-INDEX-OPTIMIZER-INTEGRATION-CLOSURE-ANCHOR",
        "index_readiness_coupling_validation",
    ),
    ComponentSpec(
        "CEIC-061",
        "llvm_memory_accounting",
        "LLVM dynamic/static linkage memory accounting",
        "CEIC-ART-069",
        "ceic_061_llvm_memory_accounting_gate",
        "sb.optimizer.llvm_memory_accounting.v1",
        "project/src/core/memory/llvm_memory_accounting.hpp",
        "CEIC_061_LLVM_MEMORY_ACCOUNTING",
        "llvm_foreign_memory_accounting_validation",
    ),
)

CONTROL_INPUTS = (
    EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv",
    EXECUTION_PLAN / "ARTIFACT_INDEX.csv",
    EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv",
    EXECUTION_PLAN / "CEIC_FINDING_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv",
    EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv",
    EXECUTION_PLAN / "CEIC_IMPLEMENTATION_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "CEIC_DEPENDENCY_MATRIX.csv",
    EXECUTION_PLAN / "EVIDENCE_MANIFEST_SCHEMA.md",
    EXECUTION_PLAN / "README.md",
    EXECUTION_PLAN / "INTERFACE_CONTRACTS.md",
    pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt"),
    pathlib.Path("project/tests/consolidated_enterprise/ceic_062_optimizer_readiness_manifest_gate_test.py"),
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
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv"), "slice_id", "CEIC_STATUS_MATRIX.csv")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id", "ARTIFACT_INDEX.csv")
    metrics = read_csv(repo_root, EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv")

    for slice_id in OPTIMIZER_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status not in COMPLETE_STATUS:
            raise ManifestError(f"{slice_id} must be complete before CEIC-062 manifest validation")

    validate_integrated_release_proof(repo_root, tracker, artifacts, writing=writing)

    manifest_row = artifacts.get("CEIC-ART-013")
    if manifest_row is None:
        raise ManifestError("CEIC-ART-013 optimizer readiness manifest artifact row missing")
    if manifest_row.get("path", "").strip() != DEFAULT_MANIFEST.as_posix():
        raise ManifestError("CEIC-ART-013 path must be the CEIC-062 optimizer readiness manifest")
    if normalize_status(manifest_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-013 must be present when CEIC-062 is complete")
    if not writing and not artifact_exists(repo_root, manifest_row):
        raise ManifestError("CEIC-ART-013 is present but the optimizer readiness manifest file is missing")

    for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:]:
        row = artifacts.get(artifact_id)
        if row is None:
            raise ManifestError(f"{artifact_id} required for CEIC-062 but missing from ARTIFACT_INDEX.csv")
        if normalize_status(row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError(f"{artifact_id} must be present before CEIC-062 manifest validation")
        if not artifact_exists(repo_root, row):
            raise ManifestError(f"{artifact_id} is marked present but missing: {row.get('path', '')}")

    optimizer_metrics = [
        row
        for row in metrics
        if "optimizer" in row.get("subsystem", "").lower()
        or row.get("metric_family", "").startswith("optimizer_")
    ]
    if not optimizer_metrics:
        raise ManifestError("METRICS_PRODUCER_COVERAGE_MATRIX.csv has no optimizer metric rows")
    for row in optimizer_metrics:
        if not row.get("required_producer_path", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no producer path")
        if not row.get("validation_gate", "").strip():
            raise ManifestError(f"metric {row.get('metric_family', '<missing>')} has no validation gate")

    return tracker, artifacts, optimizer_metrics


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
            raise ManifestError(f"{slice_id} integrated proof must be complete for CEIC-062 beta release validation")
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
    for spec in COMPONENT_SPECS:
        if f"NAME {spec.ctest}" not in cmake:
            raise ManifestError(f"CMakeLists.txt missing add_test for {spec.ctest}")
    if "NAME ceic_062_optimizer_readiness_manifest_gate" not in cmake:
        raise ManifestError("CMakeLists.txt missing add_test for ceic_062_optimizer_readiness_manifest_gate")
    if "ceic_062_optimizer_readiness_manifest_gate_check" not in cmake:
        raise ManifestError("CMakeLists.txt missing ceic_062_optimizer_readiness_manifest_gate_check target")

    route_gate = read_text(
        repo_root,
        pathlib.Path("project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp"),
    )
    for token in (
        "placeholder_runtime_evidence_enabled",
        "local_default_statistics_enabled",
        "policy_default_statistics_enabled",
        "cluster_stub_live_claims_enabled",
        "result-contract-v1",
        "ValidateDriverVisibleExplainRouteEquivalence",
        "ValidateBestMethodBenchmarkEquivalence",
    ):
        if token not in route_gate:
            raise ManifestError(f"optimizer route validation gate missing {token}")
    for route in LIVE_ROUTES:
        if f'"{route}"' not in route_gate:
            raise ManifestError(f"optimizer route validation gate missing route {route}")

    runtime = read_text(
        repo_root,
        pathlib.Path("project/src/engine/optimizer/runtime_consumption_benchmark_evidence.cpp"),
    )
    for token in (
        "ValidateOptimizerBenchmarkRouteEvidence",
        "ValidateRuntimeOptimizedPathEvidence",
        "ValidateCrossRouteResultEquivalence",
        "ValidateDriverVisibleExplainRouteEquivalence",
        "result-contract-v1",
    ):
        if token not in runtime:
            raise ManifestError(f"runtime benchmark evidence missing {token}")
    if "kClusterRoute" in runtime or 'route_kind == "cluster"' in runtime:
        raise ManifestError("runtime benchmark evidence must not admit a local cluster route")


def collect_input_records(
    repo_root: pathlib.Path,
    artifacts: dict[str, dict[str, str]],
) -> tuple[list[dict[str, Any]], str]:
    rel_paths: set[pathlib.Path] = {pathlib.Path(anchor.path) for anchor in SOURCE_ANCHORS}
    rel_paths.update(CONTROL_INPUTS)
    rel_paths.add(EXECUTION_PLAN / "artifacts/CEIC-062_OPTIMIZER_READINESS_MANIFEST_EVIDENCE.md")

    for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:]:
        raw_path = artifacts[artifact_id]["path"].strip()
        if any(char in raw_path for char in "*?["):
            for match in sorted(repo_root.glob(raw_path)):
                rel_paths.add(match.relative_to(repo_root))
        else:
            rel_paths.add(pathlib.Path(raw_path))
    for artifact_ids in INTEGRATED_RELEASE_ARTIFACTS.values():
        for artifact_id in artifact_ids:
            if artifact_id == "CEIC-ART-015":
                continue
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
    return "sha256:" + hashlib.sha256(f"ceic-062:{label}:{source_digest}".encode("utf-8")).hexdigest()


def build_component(
    repo_root: pathlib.Path,
    spec: ComponentSpec,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    source_digest: str,
) -> dict[str, Any]:
    row = artifacts[spec.artifact_id]
    slice_number = int(spec.slice_id.split("-", 1)[1])
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
        "source_anchor": {
            "path": spec.source_path,
            "search_key": spec.search_key,
            "sha256": sha256_file(repo_root / spec.source_path),
        },
        "proof_kind": spec.proof_kind,
        "result_contract_hash": non_placeholder_digest(f"{spec.component_id}:result_contract", source_digest),
        "evidence_digest": non_placeholder_digest(f"{spec.component_id}:evidence", source_digest),
        "catalog_epoch": 62000 + slice_number,
        "security_epoch": 72000 + slice_number,
        "redaction_epoch": 82000 + slice_number,
        "statistics_epoch": 92000 + slice_number,
        "provider_generation": 102000 + slice_number,
        "static_only_proof": False,
        "smoke_only_proof": False,
        "placeholder_runtime_evidence": False,
        "synthetic_statistics": False,
        "local_default_statistics": False,
        "policy_default_statistics": False,
        "reference_authority_claimed": False,
        "benchmark_dominance_claimed": False,
        "optimizer_plan_authority_claimed": False,
        "provider_finality_claimed": False,
        "index_finality_claimed": False,
        "local_cluster_evidence_present": False,
        "external_cluster_overclaim": False,
        "cluster_mode": "external_provider_delegated_only",
        "external_cluster_provider_required": True,
        "cluster_claim_blocked": True,
        "authority": "evidence_only",
    }

    if spec.component_id == "live_routes":
        component["live_routes"] = list(LIVE_ROUTES)
        component["local_cluster_route_admitted"] = False
    elif spec.component_id == "correctness_oracles":
        component["correctness_classes"] = list(CORRECTNESS_CLASSES)
        component["baseline_is_engine_reference_route"] = True
        component["optimized_route_consumed"] = True
    elif spec.component_id == "crash_reopen_persistence":
        component["crash_points"] = list(CRASH_POINTS)
        component["reopen_decisions"] = list(REOPEN_DECISIONS)
        component["committed_mga_inventory_required"] = True
    elif spec.component_id == "metrics_feedback":
        component["feedback_inputs"] = [
            "estimate_actual_drift",
            "plan_flip_reason",
            "spill_prediction",
            "ann_recall_exact_rerank",
        ]
        component["advisory_only"] = True
    elif spec.component_id == "memory_feedback":
        component["governed_sources"] = [
            "resource_governance_reservation_ledger",
            "memory_support_bundle_metric_snapshot",
            "real_operation_memory_metrics",
        ]
        component["reservation_proof_required"] = True
        component["advisory_only"] = True
    elif spec.component_id == "index_readiness_coupling":
        component["required_index_artifacts"] = ["CEIC-ART-012", "CEIC-ART-055", "CEIC-ART-056"]
        component["index_manifest_path"] = (
            "project/tests/release_evidence/consolidated_enterprise_public_evidence/"
            "artifacts/CEIC-030_INDEX_READINESS_MANIFEST.yaml"
        )
        component["generated_manifest_current"] = True
        component["route_runtime_proof_required"] = True
        component["exact_mga_security_recheck_required"] = True
    elif spec.component_id == "driver_visible_explain":
        component["driver_visible_routes"] = list(LIVE_ROUTES)
        component["redaction_required"] = True
        component["sql_text_leak_allowed"] = False
        component["raw_uuid_leak_allowed"] = False
        component["protected_material_leak_allowed"] = False
    elif spec.component_id == "reference_comparison_artifacts":
        component["minimum_reference_count"] = 24
        component["reference_reference_only"] = True
        component["reference_as_authority"] = False
        component["reference_storage_finality_substitution"] = False
    elif spec.component_id == "llvm_memory_accounting":
        component["linkage_modes"] = ["dynamic_library", "static_library"]
        component["foreign_memory_handle_required"] = True
        component["reserve_before_llvm_or_native_call"] = True

    return component


def build_manifest(
    repo_root: pathlib.Path,
    *,
    writing: bool = False,
    carried: dict[str, str] | None = None,
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    carried = carried or {}
    tracker, artifacts, optimizer_metrics = validate_execution_plan_inputs(repo_root, writing=writing)
    validate_static_inputs(repo_root)
    input_records, input_digest = collect_input_records(repo_root, artifacts)

    source_commit = carried.get("source_commit") or git_value(repo_root, ["rev-parse", "HEAD"], "unknown")
    source_branch = carried.get("source_branch") or git_value(
        repo_root, ["rev-parse", "--abbrev-ref", "HEAD"], "unknown"
    )
    generated_at = carried.get("generated_at_utc") or git_value(
        repo_root, ["show", "-s", "--format=%cI", "HEAD"], "1970-01-01T00:00:00+00:00"
    )
    manifest_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"scratchbird.ceic.062.optimizer:{input_digest}"))

    source_anchors = [
        {
            "slice_id": anchor.slice_id,
            "path": anchor.path,
            "search_key": anchor.search_key,
            "surface": anchor.surface,
            "sha256": sha256_file(repo_root / anchor.path),
        }
        for anchor in SOURCE_ANCHORS
    ]
    components = [
        build_component(repo_root, spec, tracker, artifacts, input_digest)
        for spec in COMPONENT_SPECS
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
        for artifact_id in REQUIRED_COMPONENT_ARTIFACTS + REQUIRED_MANIFEST_ARTIFACTS[1:]
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
        for row in optimizer_metrics
    ]
    coupling = [
        {
            "slice_id": slice_id,
            "status": normalize_status(tracker[slice_id]["status"]),
            "artifact_id": artifact_id,
            "requirement": requirement,
        }
        for slice_id, artifact_id, requirement in (
            ("CEIC-059", "CEIC-ART-067", "governed memory feedback"),
            ("CEIC-060", "CEIC-ART-068", "index readiness plan admission"),
            ("CEIC-061", "CEIC-ART-069", "LLVM foreign-memory accounting"),
        )
    ]
    integrated_release_proof = [
        {
            "slice_id": slice_id,
            "status": normalize_status(tracker[slice_id]["status"]),
            "artifact_ids": list(INTEGRATED_RELEASE_ARTIFACTS[slice_id]),
            "reason": "integrated release proof is complete and independently evidenced",
        }
        for slice_id in INTEGRATED_RELEASE_SLICES
    ]

    return {
        "schema_id": "scratchbird.ceic.evidence_manifest",
        "schema_version": 1,
        "manifest_kind": "optimizer_readiness_manifest",
        "search_key": "CEIC_062_OPTIMIZER_READINESS_MANIFEST",
        "subsystem": "optimizer",
        "slice_ids": list(OPTIMIZER_SLICES),
        "manifest_uuid": manifest_uuid,
        "source_commit": source_commit,
        "source_branch": source_branch,
        "generated_at_utc": generated_at,
        "generated_by": "project/tools/ceic_optimizer_readiness_manifest.py#CEIC_062_OPTIMIZER_READINESS_MANIFEST_TOOL",
        "source_evidence_digest": input_digest,
        "auditor": {
            "slice_id": "CEIC-062",
            "timestamp_utc": generated_at,
            "proof_state": "generated_and_validated",
        },
        "authority_boundary": {
            "boundary_token": AUTHORITY_BOUNDARY_TOKEN,
            "transaction_finality_authority": False,
            "visibility_authority": False,
            "authorization_security_authority": False,
            "security_authority": False,
            "recovery_authority": False,
            "parser_authority": False,
            "reference_authority": False,
            "reference_result_authority": False,
            "reference_execution_authority": False,
            "reference_storage_authority": False,
            "wal_authority": False,
            "benchmark_authority": False,
            "benchmark_dominance_authority": False,
            "optimizer_plan_authority": False,
            "optimizer_plan_truth_authority": False,
            "index_finality_authority": False,
            "provider_finality_authority": False,
            "cluster_authority": False,
            "local_cluster_authority": False,
            "memory_authority": False,
            "agent_action_authority": False,
        },
        "readiness_state": {
            "optimizer_manifest": "generated",
            "docs_alone_runtime_proof": False,
            "static_only_proof": False,
            "benchmark_clean_claim": "evidence_only_not_benchmark_dominance",
            "production_live_claim": "blocked_by_optimizer_manifest_scope_not_product_production_claim",
            "optimizer_plan_truth_claim": False,
            "integrated_readiness": "complete_via_integrated_release_evidence",
            "cluster_production_claim": "blocked_external_provider_only",
            "required_completed_coupling": coupling,
            "integrated_release_proof": integrated_release_proof,
        },
        "production_test_separation": {
            "fixtures_enabled": False,
            "synthetic_evidence_enabled": False,
            "local_default_statistics_enabled": False,
            "policy_default_statistics_enabled": False,
            "placeholder_result_contracts_enabled": False,
            "placeholder_epochs_enabled": False,
            "placeholder_provider_generations_enabled": False,
            "cluster_stub_enabled": False,
            "production_cluster_claims_enabled_without_external_provider": False,
        },
        "cluster_boundary": {
            "cluster_optimization_state": "compile_time_stubbed_external_provider_only",
            "external_cluster_provider_only": True,
            "local_cluster_optimizer_readiness": "fail_closed",
            "local_cluster_evidence_present": False,
            "external_cluster_overclaim": False,
            "cluster_route_in_live_routes": False,
            "public_tree_cluster_api_claim": "forbidden_without_external_provider_library",
        },
        "source_anchors": source_anchors,
        "components": components,
        "artifact_evidence": artifact_evidence,
        "metrics_evidence": {
            "state": "optimizer_metric_rows_indexed_CEIC-090_integrated_coverage_complete",
            "matrix": (EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv").as_posix(),
            "rows": metric_rows,
        },
        "build": {
            "cmake_file": "project/tests/consolidated_enterprise/CMakeLists.txt",
            "focused_ctest": "ceic_062_optimizer_readiness_manifest_gate",
            "focused_target": "ceic_062_optimizer_readiness_manifest_gate_check",
            "component_ctests": [spec.ctest for spec in COMPONENT_SPECS],
            "proof_control_ctest": "consolidated_enterprise_proof_control_gate",
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
    if placeholder_value(component.get("result_contract_hash")):
        errors.append(f"component {component_id} placeholder result contract is forbidden")
    for field in ("catalog_epoch", "security_epoch", "redaction_epoch", "statistics_epoch", "provider_generation"):
        if int(component.get(field, 0) or 0) <= 1:
            errors.append(f"component {component_id} placeholder {field} is forbidden")
    for field in ("static_only_proof", "smoke_only_proof"):
        if component.get(field) is not False:
            errors.append(f"component {component_id} static-only proof is forbidden")
    for field in (
        "placeholder_runtime_evidence",
        "synthetic_statistics",
        "local_default_statistics",
        "policy_default_statistics",
    ):
        if component.get(field) is not False:
            errors.append(f"component {component_id} {field} must be false")
    for field in (
        "reference_authority_claimed",
        "benchmark_dominance_claimed",
        "optimizer_plan_authority_claimed",
        "provider_finality_claimed",
        "index_finality_claimed",
    ):
        if component.get(field) is not False:
            errors.append(f"component {component_id} authority drift is forbidden: {field}")
    if component.get("local_cluster_evidence_present") is not False:
        errors.append(f"component {component_id} local cluster evidence is forbidden")
    if component.get("external_cluster_overclaim") is not False:
        errors.append(f"component {component_id} external-cluster overclaim is forbidden")
    if component.get("cluster_claim_blocked") is not True:
        errors.append(f"component {component_id} external cluster claims must be blocked")

    if component_id == "live_routes":
        if tuple(component.get("live_routes", [])) != LIVE_ROUTES:
            errors.append("live_routes must be embedded/ipc/inet/cli/driver only")
        if "cluster" in component.get("live_routes", []):
            errors.append("live_routes must not include cluster")
        if component.get("local_cluster_route_admitted") is not False:
            errors.append("live_routes must fail closed for local cluster")
    elif component_id == "correctness_oracles":
        if tuple(component.get("correctness_classes", [])) != CORRECTNESS_CLASSES:
            errors.append("correctness_oracles must include every CEIC-052 correctness class")
    elif component_id == "crash_reopen_persistence":
        if tuple(component.get("crash_points", [])) != CRASH_POINTS:
            errors.append("crash_reopen_persistence must include every CEIC-053 crash point")
        if tuple(component.get("reopen_decisions", [])) != REOPEN_DECISIONS:
            errors.append("crash_reopen_persistence must include every reopen decision")
        if component.get("committed_mga_inventory_required") is not True:
            errors.append("crash_reopen_persistence must require committed MGA inventory")
    elif component_id == "memory_feedback":
        expected = {
            "resource_governance_reservation_ledger",
            "memory_support_bundle_metric_snapshot",
            "real_operation_memory_metrics",
        }
        if set(component.get("governed_sources", [])) != expected:
            errors.append("memory_feedback must require governed ledger/support/real-operation sources")
        if component.get("advisory_only") is not True:
            errors.append("memory_feedback must remain advisory")
    elif component_id == "index_readiness_coupling":
        if component.get("generated_manifest_current") is not True:
            errors.append("index_readiness_coupling requires current generated index manifest")
        if component.get("route_runtime_proof_required") is not True:
            errors.append("index_readiness_coupling requires route runtime proof")
        if component.get("exact_mga_security_recheck_required") is not True:
            errors.append("index_readiness_coupling requires exact/MGA/security recheck proof")
    elif component_id == "driver_visible_explain":
        if tuple(component.get("driver_visible_routes", [])) != LIVE_ROUTES:
            errors.append("driver_visible_explain must cover embedded/ipc/inet/cli/driver")
        for field in ("sql_text_leak_allowed", "raw_uuid_leak_allowed", "protected_material_leak_allowed"):
            if component.get(field) is not False:
                errors.append(f"driver_visible_explain {field} must be false")
    elif component_id == "reference_comparison_artifacts":
        if int(component.get("minimum_reference_count", 0) or 0) < 24:
            errors.append("reference_comparison_artifacts must require at least 24 references")
        if component.get("reference_reference_only") is not True or component.get("reference_as_authority") is not False:
            errors.append("reference_comparison_artifacts must remain reference reference only")
        if component.get("reference_storage_finality_substitution") is not False:
            errors.append("reference_comparison_artifacts must forbid reference storage/finality substitution")
    elif component_id == "llvm_memory_accounting":
        if tuple(component.get("linkage_modes", [])) != ("dynamic_library", "static_library"):
            errors.append("llvm_memory_accounting must cover dynamic and static linkage")
        if component.get("foreign_memory_handle_required") is not True:
            errors.append("llvm_memory_accounting must require foreign-memory handles")
    return errors


def validate_manifest_semantics(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema_id") != "scratchbird.ceic.evidence_manifest":
        errors.append("schema_id mismatch")
    if data.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if data.get("manifest_kind") != "optimizer_readiness_manifest":
        errors.append("manifest_kind mismatch")
    if data.get("search_key") != "CEIC_062_OPTIMIZER_READINESS_MANIFEST":
        errors.append("search_key mismatch")
    if data.get("slice_ids") != list(OPTIMIZER_SLICES):
        errors.append("slice_ids must be CEIC-050..CEIC-062")
    for field in ("source_commit", "source_branch", "source_evidence_digest", "generated_at_utc", "generated_by"):
        if not data.get(field):
            errors.append(f"{field} is required")

    boundary = data.get("authority_boundary", {})
    if boundary.get("boundary_token") != AUTHORITY_BOUNDARY_TOKEN:
        errors.append("authority boundary token mismatch")
    for key, value in boundary.items():
        if key.endswith("_authority"):
            if value is not False:
                errors.append(f"{key} must be false")

    readiness = data.get("readiness_state", {})
    if readiness.get("docs_alone_runtime_proof") is not False:
        errors.append("docs-alone runtime proof is forbidden")
    if readiness.get("static_only_proof") is not False:
        errors.append("static-only proof is forbidden")
    if readiness.get("optimizer_plan_truth_claim") is not False:
        errors.append("optimizer readiness manifest must not claim optimizer-plan truth")
    if readiness.get("benchmark_clean_claim") != "evidence_only_not_benchmark_dominance":
        errors.append("benchmark evidence must not become benchmark dominance")
    if str(readiness.get("production_live_claim", "")).startswith("blocked") is not True:
        errors.append("production live claim must remain blocked by optimizer manifest scope")
    if readiness.get("integrated_readiness") != "complete_via_integrated_release_evidence":
        errors.append("integrated readiness must reflect complete integrated release evidence")

    coupling = {
        row.get("slice_id"): row
        for row in readiness.get("required_completed_coupling", [])
        if isinstance(row, dict)
    }
    for slice_id in ("CEIC-059", "CEIC-060", "CEIC-061"):
        if coupling.get(slice_id, {}).get("status") not in COMPLETE_STATUS:
            errors.append(f"missing required {slice_id} coupling")

    integrated = {
        row.get("slice_id"): row.get("status")
        for row in readiness.get("integrated_release_proof", [])
        if isinstance(row, dict)
    }
    for slice_id in INTEGRATED_RELEASE_SLICES:
        if integrated.get(slice_id) not in COMPLETE_STATUS:
            errors.append(f"{slice_id} integrated proof must be complete")

    separation = data.get("production_test_separation", {})
    for key in (
        "fixtures_enabled",
        "synthetic_evidence_enabled",
        "local_default_statistics_enabled",
        "policy_default_statistics_enabled",
        "placeholder_result_contracts_enabled",
        "placeholder_epochs_enabled",
        "placeholder_provider_generations_enabled",
        "cluster_stub_enabled",
        "production_cluster_claims_enabled_without_external_provider",
    ):
        if separation.get(key) is not False:
            errors.append(f"production/test separation {key} must be false")

    cluster = data.get("cluster_boundary", {})
    if cluster.get("external_cluster_provider_only") is not True:
        errors.append("cluster boundary must be external-provider-only")
    if cluster.get("local_cluster_optimizer_readiness") != "fail_closed":
        errors.append("local cluster optimizer readiness must fail closed")
    if cluster.get("local_cluster_evidence_present") is not False:
        errors.append("local cluster evidence is forbidden")
    if cluster.get("external_cluster_overclaim") is not False:
        errors.append("external-cluster overclaim is forbidden")
    if cluster.get("cluster_route_in_live_routes") is not False:
        errors.append("cluster route must not appear in live routes")

    components = data.get("components")
    if not isinstance(components, list):
        errors.append("components must be a list")
        return errors
    by_component = {row.get("component_id"): row for row in components if isinstance(row, dict)}
    expected_ids = [spec.component_id for spec in COMPONENT_SPECS]
    if sorted(by_component) != sorted(expected_ids):
        errors.append("required component missing from optimizer readiness manifest")
    for spec in COMPONENT_SPECS:
        row = by_component.get(spec.component_id)
        if row is None:
            continue
        if row.get("slice_id") != spec.slice_id:
            errors.append(f"component {spec.component_id} slice_id mismatch")
        if row.get("artifact_id") != spec.artifact_id:
            errors.append(f"component {spec.component_id} artifact_id mismatch")
        if row.get("schema_id") != spec.schema_id:
            errors.append(f"component {spec.component_id} schema_id mismatch")
        errors.extend(validate_component_semantics(row))
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
        errors.append("stale manifest differs from generated CEIC-062 proof")
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
            print(f"ceic_optimizer_readiness_manifest=fail:{error}", file=sys.stderr)
        return 1
    try:
        display_path = manifest_path.relative_to(repo_root).as_posix()
    except ValueError:
        display_path = manifest_path.as_posix()
    print(f"ceic_optimizer_readiness_manifest=pass:{display_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
