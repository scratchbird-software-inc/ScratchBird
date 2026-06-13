#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and generate the CEIC-095 final enterprise proof manifest.

SEARCH_KEY: CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any


EXECUTION_PLAN_NAME = "consolidated_enterprise_public_evidence"
ACTIVE_EXECUTION_PLAN = pathlib.Path("project/tests/release_evidence") / EXECUTION_PLAN_NAME
COMPLETED_EXECUTION_PLAN = ACTIVE_EXECUTION_PLAN
DEFAULT_MANIFEST = pathlib.Path("artifacts/ceic/integrated/final_enterprise_manifest.yaml")
CMAKE_GATE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")

REQUIRED_PACKAGE_FILES = (
    "README.md",
    "CEIC_STATUS_MATRIX.csv",
    "CEIC_ACCEPTANCE_MATRIX.csv",
    "CEIC_DEPENDENCY_MATRIX.csv",
    "CEIC_IMPLEMENTATION_TRACEABILITY_MATRIX.csv",
    "CEIC_VALIDATION_PROTOCOL.md",
    "CEIC_PROOF_SEQUENCE.md",
    "CLAIM_BOUNDARY_MATRIX.csv",
    "EVIDENCE_MANIFEST_SCHEMA.md",
    "CEIC_ACCEPTANCE_CRITERIA.md",
    "CEIC_RISK_MATRIX.csv",
    "CEIC_DECISION_MATRIX.md",
    "ARTIFACT_INDEX.csv",
    "CEIC_ROLLBACK_AND_MIGRATION_PROCEDURE.md",
    "EXECUTION_PROTOCOL.md",
    "CEIC_FINDING_TRACEABILITY_MATRIX.csv",
    "CEIC_PUBLIC_READINESS_SUMMARY_TEMPLATE.md",
    "CEIC_PUBLIC_READINESS_SUMMARY.md",
    "METRICS_PRODUCER_COVERAGE_MATRIX.csv",
    "INTERFACE_CONTRACTS.md",
)
REQUIRED_MANIFEST_ARTIFACTS = (
    "CEIC-ART-011",
    "CEIC-ART-012",
    "CEIC-ART-013",
    "CEIC-ART-014",
    "CEIC-ART-015",
)
REQUIRED_INTEGRATED_ARTIFACTS = (
    "CEIC-ART-087",
    "CEIC-ART-088",
    "CEIC-ART-089",
    "CEIC-ART-090",
    "CEIC-ART-091",
)
REQUIRED_INTEGRATED_SLICES = ("CEIC-090", "CEIC-091", "CEIC-092", "CEIC-093", "CEIC-094", "CEIC-095")
COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUSES = {"present", "complete", "completed", "generated"}
AVAILABLE_STATUSES = {"available", "present", "complete", "completed"}
RISK_FINAL_PREFIXES = ("accepted", "closed")
FORBIDDEN_ROW_STATUSES = {
    "pending",
    "planned",
    "defined",
    "defined_only",
    "contract_only",
    "descriptor_only",
    "descriptor_only_complete",
    "static_only",
    "static_only_complete",
    "anchor_only",
    "sidecar_only",
    "stub",
    "stubbed",
    "fixture_only",
    "test_only",
}
AUTHORITY_FLAGS = (
    "runtime_authority",
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
    "index_finality_authority",
    "provider_authority",
    "cluster_authority",
    "local_cluster_authority",
    "memory_authority",
    "support_bundle_authority",
    "agent_action_authority",
)
NO_AUTHORITY_TOKEN = (
    "final_manifest_is_audit_evidence_only_not_transaction_finality_visibility_authorization_"
    "security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_memory_"
    "provider_cluster_support_bundle_or_agent_action_authority"
)
LOCAL_CLUSTER_POSITIVE_RE = re.compile(
    r"local[_ -]cluster[_ -]production[_ -]claim[\"']?\s*[:=]\s*(?:true|yes|1)",
    re.IGNORECASE,
)
STATIC_ONLY_POSITIVE_RE = re.compile(
    r"\b(?:static|descriptor|anchor|sidecar|contract)[_-]?only[_ -]complete\b",
    re.IGNORECASE,
)
SUCCESSOR_RE = re.compile(r"\bCEIC-(?:09[6-9]|[1-9][0-9]{3,})\b")


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


def is_complete(value: str) -> bool:
    status = normalize_status(value)
    return status in COMPLETE_STATUSES or status.startswith("complete_")


def split_refs(value: str) -> list[str]:
    refs: list[str] = []
    for raw in (value or "").replace(",", ";").split(";"):
        ref = raw.strip()
        if ref and ref.lower() not in {"none", "n/a", "na"}:
            refs.append(ref)
    return refs


def render_json(data: Any) -> str:
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def sha256_bytes(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    return sha256_bytes(path.read_bytes())


def git_value(repo_root: pathlib.Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return "unknown"
    return result.stdout.strip() or "unknown"


def resolve_execution_plan(repo_root: pathlib.Path, override: pathlib.Path | None = None) -> pathlib.Path:
    if override is not None:
        candidate = override if override.is_absolute() else repo_root / override
        if candidate.exists():
            return candidate.relative_to(repo_root) if candidate.is_relative_to(repo_root) else candidate
        raise FileNotFoundError(str(override))
    if (repo_root / ACTIVE_EXECUTION_PLAN).exists():
        return ACTIVE_EXECUTION_PLAN
    if COMPLETED_EXECUTION_PLAN != ACTIVE_EXECUTION_PLAN and (repo_root / COMPLETED_EXECUTION_PLAN).exists():
        return COMPLETED_EXECUTION_PLAN
    raise FileNotFoundError(str(ACTIVE_EXECUTION_PLAN))


def alias_execution_plan_path(rel: pathlib.Path, execution_plan_root: pathlib.Path) -> pathlib.Path | None:
    text = rel.as_posix()
    active = ACTIVE_EXECUTION_PLAN.as_posix()
    completed = COMPLETED_EXECUTION_PLAN.as_posix()
    selected = execution_plan_root.as_posix()
    if text.startswith(active + "/"):
        return pathlib.Path(selected) / text[len(active) + 1 :]
    if completed != active and text.startswith(completed + "/"):
        return pathlib.Path(selected) / text[len(completed) + 1 :]
    return None


def resolve_repo_path(repo_root: pathlib.Path, rel_text: str, execution_plan_root: pathlib.Path) -> pathlib.Path | None:
    if not rel_text.strip():
        return None
    rel = pathlib.Path(rel_text)
    if rel.is_absolute():
        return rel if rel.exists() else None
    direct = repo_root / rel
    if direct.exists():
        return direct
    alias = alias_execution_plan_path(rel, execution_plan_root)
    if alias is not None:
        aliased = repo_root / alias if not alias.is_absolute() else alias
        if aliased.exists():
            return aliased
    return None


def path_exists(repo_root: pathlib.Path, rel_text: str, execution_plan_root: pathlib.Path) -> bool:
    if any(char in rel_text for char in "*?["):
        if any(repo_root.glob(rel_text)):
            return True
        alias = alias_execution_plan_path(pathlib.Path(rel_text), execution_plan_root)
        return alias is not None and any(repo_root.glob(alias.as_posix()))
    return resolve_repo_path(repo_root, rel_text, execution_plan_root) is not None


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


def read_execution_plan_csv(repo_root: pathlib.Path, execution_plan_root: pathlib.Path, name: str) -> list[dict[str, str]]:
    path = repo_root / execution_plan_root / name if not execution_plan_root.is_absolute() else execution_plan_root / name
    return read_csv(path)


def index_by(rows: list[dict[str, str]], field: str) -> dict[str, dict[str, str]]:
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def artifact_available(
    repo_root: pathlib.Path,
    execution_plan_root: pathlib.Path,
    artifact_rows: dict[str, dict[str, str]],
    artifact_id: str,
) -> bool:
    row = artifact_rows.get(artifact_id)
    if row is None:
        return False
    return normalize_status(row.get("status", "")) in PRESENT_STATUSES and path_exists(
        repo_root, row.get("path", ""), execution_plan_root
    )


def load_tables(repo_root: pathlib.Path, execution_plan_root: pathlib.Path) -> dict[str, list[dict[str, str]]]:
    return {
        "tracker": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_STATUS_MATRIX.csv"),
        "gates": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_ACCEPTANCE_MATRIX.csv"),
        "artifacts": read_execution_plan_csv(repo_root, execution_plan_root, "ARTIFACT_INDEX.csv"),
        "risks": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_RISK_MATRIX.csv"),
        "trace": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_FINDING_TRACEABILITY_MATRIX.csv"),
        "audit": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_IMPLEMENTATION_TRACEABILITY_MATRIX.csv"),
        "dependencies": read_execution_plan_csv(repo_root, execution_plan_root, "CEIC_DEPENDENCY_MATRIX.csv"),
        "claims": read_execution_plan_csv(repo_root, execution_plan_root, "CLAIM_BOUNDARY_MATRIX.csv"),
    }


def status_counts(rows: list[dict[str, str]], field: str = "status") -> dict[str, int]:
    counts: Counter[str] = Counter(normalize_status(row.get(field, "")) for row in rows)
    return dict(sorted(counts.items()))


def phase_summary(tracker_rows: list[dict[str, str]]) -> dict[str, dict[str, int]]:
    by_phase: dict[str, dict[str, int]] = defaultdict(lambda: {"total": 0, "complete": 0})
    for row in tracker_rows:
        phase = row.get("phase", "").strip() or "unknown"
        by_phase[phase]["total"] += 1
        if is_complete(row.get("status", "")):
            by_phase[phase]["complete"] += 1
    return dict(sorted(by_phase.items()))


def artifact_manifest_row(
    repo_root: pathlib.Path,
    execution_plan_root: pathlib.Path,
    artifact_rows: dict[str, dict[str, str]],
    artifact_id: str,
) -> dict[str, Any]:
    row = artifact_rows.get(artifact_id, {})
    path_text = row.get("path", "")
    resolved = resolve_repo_path(repo_root, path_text, execution_plan_root)
    if artifact_id == "CEIC-ART-015":
        artifact_hash = "sha256:self-referential-final-manifest-omitted"
    else:
        artifact_hash = sha256_file(resolved) if resolved is not None and resolved.is_file() else ""
    return {
        "artifact_id": artifact_id,
        "slice_id": row.get("slice_id", ""),
        "path": path_text,
        "status": normalize_status(row.get("status", "")),
        "exists": resolved is not None,
        "sha256": artifact_hash,
    }


def cmake_inclusion(repo_root: pathlib.Path) -> dict[str, dict[str, Any]]:
    cmake_path = repo_root / CMAKE_GATE
    text = cmake_path.read_text(encoding="utf-8") if cmake_path.exists() else ""
    expected = {
        "CEIC-090": ("ceic_090_metrics_producer_coverage_gate_check", "ceic_090_metrics_producer_coverage_gate"),
        "CEIC-091": ("ceic_091_integrated_support_bundle_gate_check", "ceic_091_integrated_support_bundle_gate"),
        "CEIC-092": ("ceic_092_route_chain_gate_check", "ceic_092_route_chain_gate"),
        "CEIC-093": ("ceic_093_reliability_security_suite_gate_check", "ceic_093_reliability_security_suite_gate"),
        "CEIC-094": ("ceic_094_cross_platform_release_gate_check", "ceic_094_cross_platform_release_gate"),
        "CEIC-095": ("ceic_095_final_enterprise_manifest_gate_check", "ceic_095_final_enterprise_manifest_gate"),
    }
    return {
        slice_id: {
            "target": target,
            "test": test,
            "target_present": target in text,
            "test_present": test in text,
            "cmake_file": CMAKE_GATE.as_posix(),
        }
        for slice_id, (target, test) in expected.items()
    }


def source_evidence(
    repo_root: pathlib.Path,
    execution_plan_root: pathlib.Path,
    artifact_rows: list[dict[str, str]],
) -> tuple[list[dict[str, str]], str]:
    rows: list[dict[str, str]] = []
    seen: set[str] = set()

    def add(path_text: str, label: str) -> None:
        if not path_text or path_text in seen or path_text == DEFAULT_MANIFEST.as_posix():
            return
        seen.add(path_text)
        resolved = resolve_repo_path(repo_root, path_text, execution_plan_root)
        if resolved is None or not resolved.is_file():
            return
        rows.append(
            {
                "label": label,
                "path": path_text,
                "sha256": sha256_file(resolved),
            }
        )

    for name in REQUIRED_PACKAGE_FILES:
        add((execution_plan_root / name).as_posix(), f"package:{name}")
    for artifact in artifact_rows:
        add(artifact.get("path", ""), f"artifact:{artifact.get('artifact_id', '')}")
    add(CMAKE_GATE.as_posix(), "cmake:consolidated_enterprise")
    add("project/tools/ceic_final_enterprise_manifest.py", "tool:ceic095")
    add("project/tests/consolidated_enterprise/ceic_095_final_enterprise_manifest_gate_test.py", "test:ceic095")

    rows.sort(key=lambda item: (item["label"], item["path"]))
    digest_input = render_json(rows).encode("utf-8")
    return rows, sha256_bytes(digest_input)


def authority_boundary() -> dict[str, Any]:
    return {
        "boundary_token": NO_AUTHORITY_TOKEN,
        **{flag: False for flag in AUTHORITY_FLAGS},
        "cluster_production_claim": "blocked_without_signed_external_provider_proof",
        "local_cluster_stub_free": True,
        "reference_comparison_authority": False,
        "static_or_descriptor_only_authority": False,
    }


def build_manifest(repo_root: pathlib.Path, execution_plan_root: pathlib.Path) -> dict[str, Any]:
    tables = load_tables(repo_root, execution_plan_root)
    artifacts_by_id = index_by(tables["artifacts"], "artifact_id")
    source_rows, evidence_digest = source_evidence(repo_root, execution_plan_root, tables["artifacts"])

    return {
        "schema_id": "scratchbird.ceic.final_enterprise_manifest",
        "schema_version": 1,
        "manifest_kind": "final_enterprise_manifest",
        "search_key": "CEIC_095_FINAL_ENTERPRISE_MANIFEST",
        "slice_id": "CEIC-095",
        "gate_id": "CEIC-GATE-900",
        "verdict": "complete_move_ready",
        "execution_plan_name": EXECUTION_PLAN_NAME,
        "execution_plan_path": execution_plan_root.as_posix(),
        "completed_execution_plan_path": COMPLETED_EXECUTION_PLAN.as_posix(),
        "source_commit": git_value(repo_root, "rev-parse", "HEAD"),
        "source_branch": git_value(repo_root, "rev-parse", "--abbrev-ref", "HEAD"),
        "generated_by": "project/tools/ceic_final_enterprise_manifest.py#CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE",
        "source_evidence_digest": evidence_digest,
        "source_evidence": source_rows,
        "tracker_summary": {
            "status_counts": status_counts(tables["tracker"]),
            "phase_summary": phase_summary(tables["tracker"]),
            "total": len(tables["tracker"]),
        },
        "acceptance_gate_summary": {
            "status_counts": status_counts(tables["gates"]),
            "total": len(tables["gates"]),
        },
        "artifact_summary": {
            "status_counts": status_counts(tables["artifacts"]),
            "total": len(tables["artifacts"]),
            "required_manifests": [
                artifact_manifest_row(repo_root, execution_plan_root, artifacts_by_id, artifact_id)
                for artifact_id in REQUIRED_MANIFEST_ARTIFACTS
            ],
            "integrated_artifacts": [
                artifact_manifest_row(repo_root, execution_plan_root, artifacts_by_id, artifact_id)
                for artifact_id in REQUIRED_INTEGRATED_ARTIFACTS
            ],
        },
        "risk_summary": {
            "status_counts": status_counts(tables["risks"]),
            "residual_risks": [
                {
                    "risk_id": row.get("risk_id", ""),
                    "area": row.get("area", ""),
                    "status": normalize_status(row.get("status", "")),
                    "disposition": "accepted_or_closed_explicitly",
                }
                for row in tables["risks"]
            ],
        },
        "traceability_summary": {
            "status_counts": status_counts(tables["trace"]),
            "x_008": index_by(tables["trace"], "finding_id").get("X-008", {}),
        },
        "implementation_audit_summary": {
            "status_counts": status_counts(tables["audit"]),
            "ceic_aud_053": index_by(tables["audit"], "audit_id").get("CEIC-AUD-053", {}),
        },
        "dependency_summary": {
            "status_counts": status_counts(tables["dependencies"]),
        },
        "claim_boundaries": tables["claims"],
        "authority_boundary": authority_boundary(),
        "cluster_boundary": {
            "cluster_production_claim": "blocked",
            "local_public_stub_cluster_claims": "forbidden",
            "external_provider_required": True,
            "local_stub_free": True,
        },
        "static_claim_boundary": {
            "static_only_claims_allowed": False,
            "descriptor_only_claims_allowed": False,
            "anchor_only_claims_allowed": False,
            "smoke_only_claims_allowed": False,
        },
        "successor_boundary": {
            "ceic_successor_overclaim": False,
            "successor_slice_ids_claimed": [],
        },
        "cmake_inclusion": cmake_inclusion(repo_root),
        "validation_commands": [
            {
                "command": "python3 project/tools/ceic_final_enterprise_manifest.py --repo-root .",
                "expected_result": "ceic_final_enterprise_manifest_gate=pass",
            },
            {
                "command": "python3 project/tests/consolidated_enterprise/ceic_095_final_enterprise_manifest_gate_test.py --repo-root .",
                "expected_result": "ceic_095_final_enterprise_manifest_gate_test=pass",
            },
            {
                "command": "python3 project/tools/consolidated_enterprise_proof_gate.py --repo-root .",
                "expected_result": "consolidated_enterprise_proof_gate=pass",
            },
        ],
    }


def validate_model(model: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if model.get("schema_id") != "scratchbird.ceic.final_enterprise_manifest":
        diagnostics.append(Diagnostic("schema", "model", "unsupported schema_id"))
    if model.get("manifest_kind") != "final_enterprise_manifest":
        diagnostics.append(Diagnostic("schema", "model", "manifest_kind must be final_enterprise_manifest"))
    if model.get("slice_id") != "CEIC-095" or model.get("gate_id") != "CEIC-GATE-900":
        diagnostics.append(Diagnostic("schema", "model", "CEIC-095 / CEIC-GATE-900 identifiers required"))
    if model.get("verdict") != "complete_move_ready":
        diagnostics.append(Diagnostic("verdict", "model", "verdict must be complete_move_ready"))
    if not str(model.get("source_evidence_digest", "")).startswith("sha256:"):
        diagnostics.append(Diagnostic("source_evidence", "model", "source evidence digest is required"))

    tracker_counts = model.get("tracker_summary", {}).get("status_counts", {})
    if set(tracker_counts) != {"complete"}:
        diagnostics.append(Diagnostic("tracker_status", "model", f"tracker status counts must be only complete: {tracker_counts}"))
    gate_counts = model.get("acceptance_gate_summary", {}).get("status_counts", {})
    if set(gate_counts) != {"complete"}:
        diagnostics.append(Diagnostic("gate_status", "model", f"gate status counts must be only complete: {gate_counts}"))
    artifact_counts = model.get("artifact_summary", {}).get("status_counts", {})
    if set(artifact_counts) != {"present"}:
        diagnostics.append(Diagnostic("artifact_status", "model", f"artifact status counts must be only present: {artifact_counts}"))

    risk_counts = model.get("risk_summary", {}).get("status_counts", {})
    for status in risk_counts:
        if not status.startswith(RISK_FINAL_PREFIXES):
            diagnostics.append(Diagnostic("risk_status", status, "risk status must start with accepted or closed"))
    for risk in model.get("risk_summary", {}).get("residual_risks", []):
        status = normalize_status(str(risk.get("status", "")))
        if not status.startswith(RISK_FINAL_PREFIXES):
            diagnostics.append(Diagnostic("risk_status", str(risk.get("risk_id", "risk")), "risk is not explicitly accepted or closed"))

    dependency_counts = model.get("dependency_summary", {}).get("status_counts", {})
    if set(dependency_counts) != {"available"}:
        diagnostics.append(Diagnostic("dependency_status", "model", f"dependencies must be available: {dependency_counts}"))
    trace_counts = model.get("traceability_summary", {}).get("status_counts", {})
    if set(trace_counts) != {"complete"}:
        diagnostics.append(Diagnostic("traceability_status", "model", f"traceability rows must be complete: {trace_counts}"))
    audit_counts = model.get("implementation_audit_summary", {}).get("status_counts", {})
    if set(audit_counts) != {"complete"}:
        diagnostics.append(Diagnostic("audit_status", "model", f"implementation audit rows must be complete: {audit_counts}"))

    for artifact in model.get("artifact_summary", {}).get("required_manifests", []):
        if artifact.get("status") != "present" or artifact.get("exists") is not True:
            diagnostics.append(Diagnostic("missing_manifest_artifact", str(artifact.get("artifact_id", "")), "required manifest artifact must be present"))
        if not str(artifact.get("sha256", "")).startswith("sha256:"):
            diagnostics.append(Diagnostic("missing_manifest_artifact", str(artifact.get("artifact_id", "")), "required manifest hash is missing"))

    authority = model.get("authority_boundary", {})
    if authority.get("boundary_token") != NO_AUTHORITY_TOKEN:
        diagnostics.append(Diagnostic("authority_boundary", "model", "boundary token mismatch"))
    for flag in AUTHORITY_FLAGS:
        if authority.get(flag) is not False:
            diagnostics.append(Diagnostic("unsafe_authority", flag, "authority flag must be false"))

    cluster = model.get("cluster_boundary", {})
    if cluster.get("cluster_production_claim") != "blocked":
        diagnostics.append(Diagnostic("local_cluster_claim", "cluster_boundary", "cluster production claim must remain blocked"))
    if cluster.get("external_provider_required") is not True or cluster.get("local_stub_free") is not True:
        diagnostics.append(Diagnostic("local_cluster_claim", "cluster_boundary", "external-provider-only local-stub-free boundary required"))

    static_boundary = model.get("static_claim_boundary", {})
    for field in (
        "static_only_claims_allowed",
        "descriptor_only_claims_allowed",
        "anchor_only_claims_allowed",
        "smoke_only_claims_allowed",
    ):
        if static_boundary.get(field) is not False:
            diagnostics.append(Diagnostic("static_only_claim", field, "static/descriptor/anchor/smoke-only claims must be forbidden"))

    successor = model.get("successor_boundary", {})
    if successor.get("ceic_successor_overclaim") is not False:
        diagnostics.append(Diagnostic("successor_overclaim", "model", "CEIC successor overclaim must be false"))
    if successor.get("successor_slice_ids_claimed"):
        diagnostics.append(Diagnostic("successor_overclaim", "model", "successor slice ids must be empty"))

    for slice_id, row in model.get("cmake_inclusion", {}).items():
        if slice_id in {"CEIC-090", "CEIC-091", "CEIC-092", "CEIC-093", "CEIC-094", "CEIC-095"}:
            if row.get("target_present") is not True or row.get("test_present") is not True:
                diagnostics.append(Diagnostic("cmake_registration", slice_id, "CMake target and CTest registration are required"))
    return diagnostics


def validate_raw_text(repo_root: pathlib.Path, execution_plan_root: pathlib.Path, manifest_path: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    paths: list[pathlib.Path] = []
    base = repo_root / execution_plan_root if not execution_plan_root.is_absolute() else execution_plan_root
    for path in base.glob("*.csv"):
        paths.append(path)
    for path in base.glob("*.md"):
        paths.append(path)
    if manifest_path.exists():
        paths.append(manifest_path)

    for path in paths:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(repo_root) if path.is_relative_to(repo_root) else path
        if LOCAL_CLUSTER_POSITIVE_RE.search(text):
            diagnostics.append(Diagnostic("local_cluster_claim", rel.as_posix(), "local cluster production claim is enabled"))
        if STATIC_ONLY_POSITIVE_RE.search(text):
            diagnostics.append(Diagnostic("static_only_claim", rel.as_posix(), "static/descriptor/anchor-only completion claim found"))
        if SUCCESSOR_RE.search(text):
            diagnostics.append(Diagnostic("successor_overclaim", rel.as_posix(), "CEIC successor slice claim found"))
    return diagnostics


def validate_execution_plan(repo_root: pathlib.Path, execution_plan_root: pathlib.Path, manifest_path: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    tables = load_tables(repo_root, execution_plan_root)
    artifacts = index_by(tables["artifacts"], "artifact_id")
    gates = index_by(tables["gates"], "gate_id")
    tracker = index_by(tables["tracker"], "slice_id")
    trace = index_by(tables["trace"], "finding_id")
    audit = index_by(tables["audit"], "audit_id")

    for name in REQUIRED_PACKAGE_FILES:
        path = repo_root / execution_plan_root / name if not execution_plan_root.is_absolute() else execution_plan_root / name
        if not path.exists():
            diagnostics.append(Diagnostic("missing_package_file", name, "required package file is absent"))

    for slice_id, row in sorted(tracker.items()):
        if not is_complete(row.get("status", "")):
            diagnostics.append(Diagnostic("tracker_status", slice_id, "tracker slice must be complete"))
    for slice_id in REQUIRED_INTEGRATED_SLICES:
        if not is_complete(tracker.get(slice_id, {}).get("status", "")):
            diagnostics.append(Diagnostic("tracker_status", slice_id, "integrated slice must be complete"))

    for gate_id, row in sorted(gates.items()):
        if not is_complete(row.get("status", "")):
            diagnostics.append(Diagnostic("gate_status", gate_id, "acceptance gate must be complete"))
    if not is_complete(gates.get("CEIC-GATE-900", {}).get("status", "")):
        diagnostics.append(Diagnostic("gate_status", "CEIC-GATE-900", "final movement gate must be complete"))

    for artifact_id, row in sorted(artifacts.items()):
        status = normalize_status(row.get("status", ""))
        if status not in PRESENT_STATUSES:
            diagnostics.append(Diagnostic("artifact_status", artifact_id, "artifact must be present"))
        if not path_exists(repo_root, row.get("path", ""), execution_plan_root):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, f"artifact path is absent: {row.get('path', '')}"))
    for artifact_id in REQUIRED_MANIFEST_ARTIFACTS + REQUIRED_INTEGRATED_ARTIFACTS:
        if not artifact_available(repo_root, execution_plan_root, artifacts, artifact_id):
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "required final artifact is absent or not present"))

    for row in tables["risks"]:
        status = normalize_status(row.get("status", ""))
        if not status.startswith(RISK_FINAL_PREFIXES):
            diagnostics.append(Diagnostic("risk_status", row.get("risk_id", ""), "residual risk must be explicitly accepted or closed"))

    for row in tables["dependencies"]:
        if normalize_status(row.get("status", "")) not in AVAILABLE_STATUSES:
            diagnostics.append(Diagnostic("dependency_status", row.get("dependency_id", ""), "dependency must be available"))

    for row in tables["trace"]:
        if not is_complete(row.get("status", "")):
            diagnostics.append(Diagnostic("traceability_status", row.get("finding_id", ""), "traceability row must be complete"))
    x_008 = trace.get("X-008", {})
    if "CEIC-095" not in x_008.get("tracker_slices", "") or "CEIC-ART-015" not in x_008.get("evidence_artifacts", ""):
        diagnostics.append(Diagnostic("traceability_status", "X-008", "X-008 must map CEIC-095 to CEIC-ART-015"))

    for row in tables["audit"]:
        if not is_complete(row.get("status", "")):
            diagnostics.append(Diagnostic("audit_status", row.get("audit_id", ""), "implementation audit row must be complete"))
    ceic_aud_053 = audit.get("CEIC-AUD-053", {})
    if "ceic_095_final_enterprise_manifest_gate" not in ceic_aud_053.get("implementation_refs", ""):
        diagnostics.append(Diagnostic("audit_status", "CEIC-AUD-053", "CEIC-095 CMake/test implementation refs are required"))

    claim_by_surface = {row.get("claim_surface", ""): row for row in tables["claims"]}
    cluster_row = claim_by_surface.get("cluster", {})
    cluster_text = " ".join(cluster_row.values()).lower()
    if "fail-closed" not in cluster_text and "fail closed" not in cluster_text:
        diagnostics.append(Diagnostic("local_cluster_claim", "cluster", "cluster claim boundary must be fail-closed"))
    if "external provider" not in cluster_text and "external-provider" not in cluster_text:
        diagnostics.append(Diagnostic("local_cluster_claim", "cluster", "cluster claim boundary must require an external provider"))
    if "public stubs" not in cluster_text or "production cluster" not in cluster_text:
        diagnostics.append(Diagnostic("local_cluster_claim", "cluster", "public cluster stubs must remain forbidden"))

    cmake_rows = cmake_inclusion(repo_root)
    for slice_id in REQUIRED_INTEGRATED_SLICES:
        row = cmake_rows.get(slice_id, {})
        if row.get("target_present") is not True or row.get("test_present") is not True:
            diagnostics.append(Diagnostic("cmake_registration", slice_id, "CMake target/test registration missing"))

    diagnostics.extend(validate_raw_text(repo_root, execution_plan_root, manifest_path))
    return diagnostics


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run(
    repo_root: pathlib.Path,
    manifest_path: pathlib.Path,
    execution_plan_override: pathlib.Path | None,
    write_manifest: bool,
    skip_execution_plan_control: bool,
) -> list[Diagnostic]:
    execution_plan_root = resolve_execution_plan(repo_root, execution_plan_override)
    manifest_abs = manifest_path if manifest_path.is_absolute() else repo_root / manifest_path

    if write_manifest:
        model = build_manifest(repo_root, execution_plan_root)
        manifest_abs.parent.mkdir(parents=True, exist_ok=True)
        manifest_abs.write_text(render_json(model), encoding="utf-8")

    if not manifest_abs.exists():
        return [Diagnostic("missing_manifest", manifest_path.as_posix(), "final manifest is absent")]

    try:
        actual = load_manifest(manifest_abs)
    except json.JSONDecodeError as exc:
        return [Diagnostic("manifest_parse", manifest_path.as_posix(), str(exc))]

    diagnostics = validate_model(actual)
    if not skip_execution_plan_control:
        diagnostics.extend(validate_execution_plan(repo_root, execution_plan_root, manifest_abs))
        expected = build_manifest(repo_root, execution_plan_root)
        if render_json(actual) != render_json(expected):
            diagnostics.append(Diagnostic("stale_manifest", manifest_path.as_posix(), "manifest differs from current generated proof"))
    return diagnostics


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--execution_plan-root", type=pathlib.Path)
    parser.add_argument("--check-only", action="store_true", help="validate existing manifest without writing")
    parser.add_argument("--write", action="store_true", help="write even when --manifest is supplied")
    parser.add_argument("--skip-execution_plan-control", action="store_true")
    parser.add_argument("--dump-default-model", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest or DEFAULT_MANIFEST
    write_manifest = args.write or (args.manifest is None and not args.check_only)
    execution_plan_root = resolve_execution_plan(repo_root, args.execution_plan_root)

    if args.dump_default_model:
        print(render_json(build_manifest(repo_root, execution_plan_root)), end="")
        return 0

    diagnostics = run(
        repo_root,
        manifest_path,
        args.execution_plan_root,
        write_manifest,
        args.skip_execution_plan_control,
    )
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"ceic_final_enterprise_manifest_gate=fail:{diagnostic.render()}", file=sys.stderr)
        return 1
    print("ceic_final_enterprise_manifest_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
